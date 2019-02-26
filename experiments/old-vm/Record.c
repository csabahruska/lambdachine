
#include "Common.h"

#if LC_HAS_JIT

#include "IR.h"
#include "Jit.h"
#include "PrintIR.h"
#include "InfoTables.h"
#include "MiscClosures.h"
#include "Thread.h"
#include "Snapshot.h"
#include "HeapInfo.h"
#include "Bitset.h"
#include "Stats.h"
#include "Opts.h"
#if LC_HAS_ASM_BACKEND
#include "AsmCodeGen.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>


/* #define DBG_PR(fmt, ...)  fprintf(stderr, fmt, __VA_ARGS__) */


// -- Forward Declarations -------------------------------------------

FragmentId registerCurrentFragment(JitState *J);
void recordCleanup(JitState *J);


// -- Convenience macros.  Undefined at end of file. -----------------

// Pointer to referenced IR.
#define IR(ref)     (&J->cur.ir[(ref)])
// The instruction currently being optimised
#define foldIns     (&J->fold.ins)

// -------------------------------------------------------------------

const u2 emptyBitmask[2] = {0, 0};

LC_FASTCALL TRef
emitLoadSlot(JitState *J, i4 slot)
{
  /* Type info is mostly needed for determining the pointerhood of
     items in snapshots.  In fact, we're really only interested in
     snapshots for guards that might trigger GC.
  */

  /* TODO: Would it be cheaper to load the type info when
     calling/returning from a function?
  */

  IRType ty;
  if (slot < 0) {
    if (slot == -1) {
      /* If slot is -1 (i.e., Node pointer) we assume it's always a
         pointer. */
      ty = IRT_CLOS;
    } else {
      /* Stack frame link info shouldn't  */
      ty = IRT_UNK;
    }
  } else {
    const u2 *ptr_mask = getParamPointerMask(getFInfo(J->T->base[-1]));
    while (slot > 15 && (*ptr_mask & 0x8000)) {
      slot -= 15;
      ptr_mask++;
    }
    if ((*ptr_mask & (1 << slot)) != 0)
      ty = IRT_CLOS;
    else
      ty = IRT_UNK;
  }
  i2 relslot = (i2)J->baseslot + slot - INITIAL_BASE;
  TRef ref = emit_raw(J, IRT(IR_SLOAD, ty), relslot, 0);
  J->base[slot] = ref;          /* Don't mark as written */
  if (slot >= J->maxslot) J->maxslot = slot + 1;
  DBG_PR("minslot: %d, baseslot: %d, slot: %d\n", J->minslot, J->baseslot, slot);
  if (J->minslot > (int)J->baseslot + slot) {
    sayonara("minslot issue");
  }
  //  if (slot >= J->maxslot) J->maxslot = slot + 1;
  return ref;
}


// Return reference location of next instruction.
//
// Grows IR buffer if needed.
INLINE_HEADER IRRef
nextIns(JitState *J)
{
  IRRef ref = J->cur.nins;
  if (LC_UNLIKELY(ref >= J->irmax)) growIRBufferTop(J);
  J->cur.nins = ref + 1;
  return ref;
}

// Write instruction from Fold engine IR into IR buffer.
LC_FASTCALL TRef
emitIR(JitState *J)
{
  IROp op = foldIns->o;

  if (ir_mode[op] & IRM_G)
    addSnapshot(J);

  IRRef ref = nextIns(J);
  IRIns *ir = IR(ref);

  // Link into per-opcode chain.
  ir->prev = J->chain[op];
  J->chain[op] = (IRRef1)ref;
  ir->o = op;
  ir->t = foldIns->t;
  ir->op1 = foldIns->op1;
  ir->op2 = foldIns->op2;
  DBG_LVL(2, COL_GREEN "emitted: %5d ", irref_int(ref));
  IF_DBG_LVL(2, printIR(&J->cur, *ir));
  DBG_LVL(2, COL_RESET);

  return TREF(ref, ir->t);
}

#define IR_INITIAL_BUF_SIZE   256

// Grow IR buffer at the top.  Makes room for more instructions to be
// inserted.
void
growIRBufferTop(JitState *J)
{
  IRIns *baseir = J->irbuf + J->irmin;
  Word szins = J->irmax - J->irmin;

  if (szins != 0) {
    baseir = realloc(baseir, 2 * szins * sizeof(IRIns));
    J->irmax = J->irmin + 2 * szins;
    DBG_PR(COLOURED(COL_BLUE, "Resizing IR buffer to: %d..%d") "\n",
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  } else {
    baseir = xmalloc(IR_INITIAL_BUF_SIZE * sizeof(IRIns));
    J->irmin = REF_BASE - IR_INITIAL_BUF_SIZE/4;
    J->irmax = J->irmin + IR_INITIAL_BUF_SIZE;
    DBG_PR(COLOURED(COL_BLUE, "Alloc new IR buf: %d..%d") "\n",
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  }
  J->cur.ir = J->irbuf = baseir - J->irmin;
}

// Grow IR buffer at the bottom.  Makes room for more literals.
// May choose to move existing content instead of resizing buffer.
static void
growIRBufferBottom(JitState *J)
{
  IRIns *baseir = J->irbuf + J->irmin;
  Word szins = J->irmax - J->irmin;
  LC_ASSERT(szins != 0);
  LC_ASSERT(J->cur.nk == J->irmin);

  if (J->cur.nins + (szins >> 1) < J->irmax) {
    // More than half of the buffer is free on top end.  Shift up by a
    // quarter.
    Word ofs = szins >> 2;
    DBG_PR(COLOURED(COL_BLUE, "Shifting IRs up by %" FMT_Word) "\n",
           ofs);
    memmove(baseir + ofs, baseir, (J->cur.nins - J->irmin) * sizeof(IRIns));
    J->irmin -= ofs;
    J->irmax -= ofs;
    J->cur.ir = J->irbuf = baseir - J->irmin;
  } else {
    // Double buffer size.
    IRIns *newbaseir = xmalloc(2 * szins * sizeof(IRIns));
    // Limit bottom growth.  Thus makes room for at most 128 new literals.
    Word ofs = szins >= 256 ? 128 : (szins >> 1);
    memcpy(newbaseir + ofs, baseir,
           (J->cur.nins - J->irmin) * sizeof(IRIns));
    free(baseir);
    J->irmin -= ofs;
    J->irmax = J->irmin + 2 * szins;
    J->cur.ir = J->irbuf = newbaseir - J->irmin;
    DBG_PR(COLOURED(COL_BLUE, "Growing IR buffer to: %d..%d\n"),
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  }
}

void
recordSetup(JitState *J, Thread *T)
{
  memset(J->slot, 0, sizeof(J->slot));
  memset(J->chain, 0, sizeof(J->chain));

  J->baseslot = INITIAL_BASE + 1; // base[baseslot] == r0, base[-1] == Node
  J->minslot = J->baseslot - 3;
  J->base = J->slot + J->baseslot;
  J->maxslot = T->top - T->base;
  J->framesize = T->top - T->base;

  J->T = T;

  // J->{pc,func} is set by recording code


  J->irmin = 0;
  J->irmax = 0;
  J->cur.mcode = NULL;
  J->cur.ir = J->irbuf = NULL;
  J->cur.nins = REF_BASE;
  J->cur.nk = REF_BASE;
  J->cur.nloop = 0;
  J->cur.nphis = 0;
  J->cur.spills = 0;
  // Emit BASE.  Causes buffer allocation.
  emit_raw(J, IRT(IR_BASE, IRT_PTR), 0, 0);
  J->last_result = 0;
  J->framedepth = 0;

  J->sizekwords = 500;
  J->cur.kwords = J->kwordsbuf = xmalloc(J->sizekwords * sizeof(Word));
  J->cur.nkwords = 0;

  J->needsnap = 0;
  J->mergesnap = 1;
  J->cur.nsnap = J->sizesnap = 0;
  J->cur.nsnapmap = J->sizesnapmap = 0;
  J->cur.snap = J->snapbuf = NULL;
  J->cur.snapmap = J->snapmapbuf = NULL;

  J->cur.nheap = J->sizeheap = 0;
  J->cur.nheapmap = J->sizeheapmap = 0;
  J->cur.heap = J->heapbuf = NULL;
  J->cur.heapmap = J->heapmapbuf = NULL;

  // J->cur.{startpc,orig} is initialised by startRecording

  J->needsnap = 0;
}

/* Returns reference for next allocated literal */
INLINE_HEADER IRRef
nextLit(JitState *J)
{
  IRRef ref = J->cur.nk;
  if (LC_UNLIKELY(ref <= J->irmin)) growIRBufferBottom(J);
  J->cur.nk = --ref;
  return ref;
}

TRef
emitKInt(JitState *J, i4 k)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  // Try to find in existing constants
  for (ref = J->chain[IR_KINT]; ref; ref = cir[ref].prev)
    if (cir[ref].i == k)
      goto found;
  ref = nextLit(J);
  ir = IR(ref);
  ir->i = k;
  ir->t = IRT_I32;
  ir->o = IR_KINT;
  ir->prev = J->chain[IR_KINT];
  J->chain[IR_KINT] = (IRRef1)ref;
  DBG_LVL(2, "emitted: %5d ", (IRRef1)ref - REF_BIAS);
  IF_DBG_LVL(2, printIR(&J->cur, *ir));
 found:
  return TREF(ref, IRT_I32);
}

LC_FASTCALL TRef
emitKWord_(JitState *J, Word w, IRType t)
{
  IRIns *ir, *cir = J->cur.ir;
  Word *kword = J->cur.kwords;
  IRRef ref;

  for (ref = J->chain[IR_KWORD]; ref; ref = cir[ref].prev)
    if (cir[ref].t == t && kword[cir[ref].u] == w)
      goto found;

  if (LC_UNLIKELY(J->cur.nkwords >= J->sizekwords)) {
    J->sizekwords *= 2;
    J->kwordsbuf = realloc(J->kwordsbuf, J->sizekwords);
    kword = J->cur.kwords = J->kwordsbuf;
  }

  ref = nextLit(J);
  ir = IR(ref);
  ir->u = J->cur.nkwords;
  J->kwordsbuf[J->cur.nkwords++] = w;
  ir->t = t;
  ir->o = IR_KWORD;
  ir->prev = J->chain[IR_KWORD];
  J->chain[IR_KWORD] = (IRRef1)ref;
  DBG_LVL(2, "emitted: %5d ", (IRRef1)ref - REF_BIAS);
  IF_DBG_LVL(2, printIR(&J->cur, *ir));
 found:
  return TREF(ref, t);
}

// Emit a constant representing a pointer relative to the trace base.
//
// This is used to build stack frames in the trace recorder.  Stack frames
// currently store:  the return address and a pointer to the previous base.
//
// For the return address we can use Word-sized constants since we're
// expecting to return to the same bytecode instruction.  The pointer
// to the previous base, however, depends on the current stack depth
// (and memory location of the stack).  We don't want to specialise on
// that.  Instead we want to store pointers relative to the value of
// `base` when entering the trace.
//
// Of course, any of these values will only end up being written to memory
// when exiting a trace.
//
LC_FASTCALL TRef
emitKBaseOffset(JitState *J, i4 offs)
{
  IRIns *ir;
  IRRef ref;

  for (ref = J->chain[IR_KBASEO]; ref; ref = IR(ref)->prev)
    if (IR(ref)->i == offs)
      goto found;

  ref = nextLit(J);
  ir = IR(ref);
  ir->i = offs;
  ir->o = IR_KBASEO;
  ir->t = IRT_PTR;
  ir->prev = J->chain[IR_KBASEO];
  J->chain[IR_KBASEO] = (IRRef1)ref;
  DBG_LVL(2, "emitted: %5d ", (IRRef1)ref - REF_BIAS);
  IF_DBG_LVL(2, printIR(&J->cur, *ir));
 found:
  return TREF(ref, IRT_PTR);
}

// Emit load of a field.
INLINE_HEADER TRef
emitFLoad(JitState *J, TRef ptr, u2 offset)
{
  TRef ref = emit(J, IRT(IR_FREF, IRT_PTR), ptr, offset);
  return emit(J, IRT(IR_FLOAD, IRT_UNK), ref, 0);
}

INLINE_HEADER TRef
emitInfoTableGuard(JitState *J, TRef clos, InfoTable *info)
{
  return 0;
}

INLINE_HEADER void
guardEqualKWord(JitState *J, TRef ref, Word k, LitType lt)
{
  TRef kref = emitKWord(J, k, lt);
  emit(J, IRT(IR_EQ, IRT_CMP), ref, kref);
}

static inline void
guardEqualRetAddr(JitState *J, TRef ref, Word ret_addr) {
  TRef kref = emitKWord(J, ret_addr, LIT_PC);
  emit(J, IRT(IR_EQRET, IRT_CMP), ref, kref);
}

static inline void
guardEqualInfo(JitState *J, TRef ref, const InfoTable *info) {
  TRef expected_info = emitKWord(J, (Word)info, LIT_INFO);
  TRef actual_info = emit(J, IRT(IR_ILOAD, IRT_INFO), ref, 0);
  emit(J, IRT(IR_EQ, IRT_CMP), actual_info, expected_info);
}

void
printSlots(JitState *J)
{
  //printf("slots: %d, %d\n", J->baseslot, J->maxslot);
  int i;
  for (i = J->minslot; i < J->baseslot + J->maxslot; i++) {
    IRRef1 ref = tref_ref(J->slot[i]);
    int written = J->slot[i] & TREF_WRITTEN;

    if (!written && ref)
      fprintf(stderr, COL_RED);

    int j = i - J->baseslot;
    if ((j & 0x03) == 0)
      fprintf(stderr, "[%d]:", j);

    if (ref == 0)
      fprintf(stderr, "---- ");
    else if (ref >= REF_BIAS)
      fprintf(stderr, "%04d ", ref - REF_BIAS);
    else
      fprintf(stderr, "K%03d ", REF_BIAS - ref);

    if (!written && ref)
      fprintf(stderr, COL_RESET);
  }
  fprintf(stderr, "\n");
}

void
printIRBuffer(JitState *J)
{
  IRRef ref;
  SnapShot *snap = J->cur.nsnap > 0 ? J->cur.snap : NULL;
  IRRef nextsnap = snap ? snap->ref : 0;
  SnapNo snapno = 0;

  fprintf(stderr, "IRs (%d..%d):\n",
         J->cur.nk - REF_BIAS,
         J->cur.nins - REF_BIAS);

  for (ref = J->cur.nk; ref < J->cur.nins; ref++) {

    // Don't print boring instructions unless we're debugging.
#if LC_DEBUG_LEVEL < 2
    if (IR(ref)->o == IR_NOP ||
        IR(ref)->o == IR_FRAME || IR(ref)->o == IR_RET)
      continue;
#endif

    if (ref == nextsnap) {
      fprintf(stderr, "     S:%02d  ",snapno);
      printSnapshot(&J->cur, snap, J->cur.snapmap);
      ++snap;
      ++snapno;
      if (snap >= J->cur.snap + J->cur.nsnap) {
        snap = NULL; nextsnap = 0;
      } else
        nextsnap = snap->ref;
    }
    printIRRef(&J->cur, ref);
    printIR(&J->cur, *IR(ref));
  }
}

int
evalNumComp(WordInt x, WordInt y, IROp op)
{
  switch (op) {
  case IR_EQ: return (x == y);
  case IR_NE: return (x != y);
  case IR_LT: return (x <  y);
  case IR_GE: return (x >= y);
  case IR_LE: return (x <= y);
  case IR_GT: return (x >  y);
  default: LC_ASSERT(0); return 0;
  }
}

void
abortRecording(JitState *J)
{
  exit(111);
}

/* Record the construction of a stack frame.
 *
 * Returns:
 *
 *   - non-zero if recording should continue
 *   - zero if recording should be aborted (e.g. too many stack frames)
 */
LC_FASTCALL int
recordBuildEvalFrame(JitState *J, TRef node, ThunkInfoTable *info,
                     const BCIns *return_pc)
{
  Word *top = J->T->top;
  u4 framesize = info->code.framesize;
  u4 t = top - J->T->base;
  u4 i, j;
  u4 b = J->T->base - J->startbase;

  if (LC_UNLIKELY(stackOverflow(J->T, top, 8 + framesize)))
    return 0;

  /* DBG_PR("CHECK: %d, %d\n", (top + 8 + framesize - J->startbase), */
  /*        MAX_SLOTS); */
  if (LC_UNLIKELY((top + 8 + framesize - J->startbase) >= MAX_SLOTS)) {
    DBG_PR("ABORT: Frame too deep (EVAL).");
    return 0;                   /* too many nested stack frames */
  }

  const u2 *liveouts = getLivenessMask(return_pc);
  fprintf(stderr, "LIVES: %p %lx\n", return_pc, (long)liveouts);

  setSlot(J, t + 0, emitKBaseOffset(J, b));
  setSlot(J, t + 1, emitKWord(J, (Word)return_pc, LIT_PC));
  setSlot(J, t + 2, emitKWord(J, (Word)stg_UPD_closure_addr, LIT_CLOSURE));
  setSlot(J, t + 3, node); // the thing to update
  setSlot(J, t + 4, 0); // undefined
  setSlot(J, t + 5, emitKBaseOffset(J, b + t + 3));
  setSlot(J, t + 6, emitKWord(J, (Word)stg_UPD_return_pc, LIT_PC));
  setSlot(J, t + 7, node);

  u2 mask;
  // Clear slots that aren't live-out.
  FOR_MASK(liveouts, mask, j, i = 0, i < t, i++) {
    if (!(mask & 1)) setSlot(J, i, 0);
  }

  DBG_PR("baseslot %d => %d (top = %d, frame = %d)\n",
         J->baseslot, J->baseslot + t + 8, t, framesize);
  J->baseslot += t + 8;
  J->base = J->slot + J->baseslot;
  J->maxslot = framesize;
  emit_raw(J, IRT(IR_FRAME, IRT_VOID), t + 8, framesize);
  for (i = 0; i < J->maxslot; i++) setSlot(J, i, 0); // clear slots
  J->framedepth += 2;
  IF_DBG_LVL(1, printSlots(J));

  return 1;
}

/* Record a single bytecode instruction (before it is being
   executed.) */
RecordResult
recordIns(JitState *J)
{
  const BCIns *pc;
  Word *tbase;
  BCIns ins;
  BCOp op;
  TRef ra, rb, rc;

  if (LC_UNLIKELY(J->pc == J->startpc && J->cur.nins > REF_FIRST)) {
    // We're back at the point where we started recording from.
    DBG_LVL(1, "Found actual loop\n");
    FragmentId id = 0;
    if (J->framedepth > 0) {
      id = finishRecording(J, UNROLL_DISABLED);
    } else if (J->framedepth < 0 && J->cur.traceType == RETURN_TRACE) {
      id = finishRecording(J, UNROLL_DISABLED);
      //sayonara("sanity check return trace");
      //      fprintf(stderr, "ABORT: Decreasing stack loop: %d\n.", J->framedepth);
      //      goto abort_recording;
    } else if (J->baseslot == INITIAL_BASE + 1) {
      id = finishRecording(J, UNROLL_ONCE);
    } else {
      id = finishRecording(J, UNROLL_DISABLED);
    }
    return (u4)REC_LOOP | ((u4)id << 8);
  }

  if (J->needsnap) {
    J->needsnap = 0;
    IF_DBG_LVL(1,
               fprintf(stderr, COL_GREEN);
               printSlots(J);
               fprintf(stderr, COL_RESET));
    addSnapshot(J);
    J->mergesnap = 1;
  }

  pc = J->pc;
  tbase = J->T->base;
  ins = *pc;
  op = bc_op(ins);

  switch (op) {

    // -- Comparisons ----------------------------------------------------

  case BC_ISLT: case BC_ISGE: case BC_ISLE: case BC_ISGT:
  case BC_ISEQ: case BC_ISNE:
    {
      DBG_PR("Emitting comparison: %s\n", ins_name[op]);
      // All treated as integer comparisions
      ra = getSlot(J, bc_a(ins));
      rc = getSlot(J, bc_d(ins));
      WordInt rav = tbase[bc_a(ins)];
      WordInt rcv = tbase[bc_d(ins)];
      int irop = (int)IR_LT + ((int)op - (int)BC_ISLT);
      // Invert condition if negative outcome
      if (!evalNumComp(rav, rcv, irop)) irop ^= 1;
      emit(J, IRT(irop, IRT_CMP), ra, rc);
    }
    break;

  case BC_MOV:
    {
      rc = getSlot(J, bc_d(ins));
      setSlot(J, bc_a(ins), rc);
    }
    break;

  case BC_MOV_RES:
    {
      if (!J->last_result)
        J->last_result = emit_raw(J, IRT(IR_RLOAD, IRT_UNK), 0, 0);
      setSlot(J, bc_a(ins), J->last_result);
      //printSlots(J);
    }
    break;

  case BC_ADDRR: case BC_SUBRR: case BC_MULRR:
  case BC_DIVRR: case BC_REMRR:
    {
      rb = getSlot(J, bc_b(ins));
      rc = getSlot(J, bc_c(ins));
      int irop = (int)IR_ADD + ((int)op - (int)BC_ADDRR);
      setSlot(J, bc_a(ins), emit(J, IRT(irop, IRT_I32), rb, rc));
    }
    break;

  case BC_JMP:
    break;

  case BC_LOADK:
    {
      u2 l = bc_d(ins);
      rb = emitKWord(J, J->func->code.lits[l], J->func->code.littypes[l]);
      setSlot(J, bc_a(ins), rb);
      //printSlots(J);
    }
    break;

  case BC_LOADFV:
    {
      rb = getSlot(J, -1);
      ra = emitFLoad(J, rb, bc_d(ins));
      setSlot(J, bc_a(ins), ra);
    }
    break;

  case BC_LOADF:
    {
      rb = getSlot(J, bc_b(ins));
      ra = emitFLoad(J, rb, bc_c(ins));
      setSlot(J, bc_a(ins), ra);
    }
    break;

  case BC_EVAL:
    {
      //DBG_PR("Recording EVAL r%d\n", bc_a(ins));
      ra = getSlot(J, bc_a(ins));
      Closure* node = (Closure*)tbase[bc_a(ins)];
      const InfoTable *ninfo = getInfo(node);

      if (closure_IND(node)) {
        goto abort_recording; // TODO: Handle indirection following.
      }

      // Specialise on info table:  Emit guard to check for same info
      // table as the one we encountered at recording time.
      guardEqualInfo(J, ra, ninfo);

      if (closure_HNF(node)) {
        // ra is in normal form.  Guard makes sure of that, so we now just
        // return.
        //
        // TODO: Actually, we may specialise a bit too much here.  It
        // depends on whether we do a case dispatch later on.
        J->last_result = ra;

      } else {
        //printSlots(J);
        DBG_LVL(2, "Recording non HNF eval\n");
        IF_DBG_LVL(2, printSlots(J));

        // Setup stack frame for evaluation.
        if (!recordBuildEvalFrame(J, ra, (ThunkInfoTable*)ninfo, J->pc + 2))
          goto abort_recording;

        IF_DBG_LVL(1, printSlots(J));
      }
    }
    break;

  case BC_CALLT:
    {
      // TODO: For now only supports exact calls and overapplication
      // on FUN functions.
      u4 nargs = bc_c(ins);
      u4 pointer_mask = bc_b(ins);
      Closure *fnode = (Closure*)tbase[bc_a(ins)];
      FuncInfoTable *info;
      u4 farity;

      if (getInfo(fnode)->type != FUN) {
        fprintf(stderr, "ABORT: CALLT with non-FUN.\n");
        goto abort_recording;
      }

      info = getFInfo(fnode);
      farity = info->code.arity;

      if (farity == nargs) {
        // Exact application

        if (LC_UNLIKELY(J->T->base + info->code.framesize >= J->startbase + MAX_SLOTS)) {
          DBG_PR("ABORT: Frame too deep.");
          goto abort_recording;
        }

        // Guard for info table, as usual
        TRef rinfo = emitKWord(J, (Word)info, LIT_INFO);
        ra = getSlot(J, bc_a(ins));
        rb = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
        emit(J, IRT(IR_EQ, IRT_CMP), rb, rinfo);
        setSlot(J, -1, ra);

        J->maxslot = info->code.framesize;

        // Invalidate non-argument slots:
        u4 i;
        for (i = nargs; i < J->maxslot; i++) setSlot(J, i, 0);
        emit_raw(J, IRT(IR_FRAME, IRT_VOID), 0, J->maxslot);

        IF_DBG_LVL(1, printSlots(J));

      } else if (farity < nargs) {

        // Overapplication
        u4 extra_args = nargs - farity;
        TRef extras[extra_args];
        int i;
        u4 topslot = extra_args + 1;
        u4 baseslot = J->T->base - J->startbase;
        u4 framesize = info->code.framesize;
        BCIns *ap_return_pc;
        Closure *ap_closure;
        DBG_LVL(2, "Recording overapplication %d + %d\n",
                farity, extra_args);
        IF_DBG_LVL(1, printSlots(J));

        getApContClosure(&ap_closure, &ap_return_pc, extra_args,
                         pointer_mask >> (nargs - extra_args));

        if (LC_UNLIKELY(J->T->base + topslot + 3 + framesize >=
                        J->startbase + MAX_SLOTS)) {
          DBG_PR("ABORT: Frame too deep.");
          goto abort_recording;
        }

        // First the guard
        TRef rinfo = emitKWord(J, (Word)info, LIT_INFO);
        ra = getSlot(J, bc_a(ins));
        rb = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
        emit(J, IRT(IR_EQ, IRT_CMP), rb, rinfo);

        // Save references of extra args.
        for (i = 0; i < extra_args; i++)
          extras[i] = getSlot(J, farity + i);

        // Set stack frame info
        setSlot(J, -1, emitKWord(J, (Word)ap_closure, LIT_CLOSURE));
        setSlot(J, topslot + 0, emitKBaseOffset(J, baseslot));
        setSlot(J, topslot + 1, emitKWord(J, (Word)ap_return_pc, LIT_PC));
        setSlot(J, topslot + 2, ra); // The function

        // Copy immediate arguments
        for (i = 0; i < nargs; i++)
          setSlot(J, topslot + 3 + i, getSlot(J, i));

        // Fill in AP continuation frame
        for (i = 0; i < extra_args; i++)
          setSlot(J, i, extras[i]);
        setSlot(J, extra_args, 0); // Clear slot, used for result

        IF_DBG_LVL(1, printSlots(J));

        // Finally, update meta info
        J->baseslot += topslot + 3;
        J->base = J->slot + J->baseslot;
        J->maxslot = framesize;
        emit_raw(J, IRT(IR_FRAME, IRT_VOID), topslot + 3, framesize);
        for (i = nargs; i < framesize; i++)
          setSlot(J, i, 0); // clear other slots
        J->framedepth++;

        IF_DBG_LVL(1, printSlots(J));

      } else {
        // Partial Application

        fprintf(stderr, "ABORT: CALLT partial application.\n");
        goto abort_recording;
      }
    }
    break;

  case BC_CALL:
    {
      u4 callargs = bc_c(ins);
      Closure *fnode = (Closure*)tbase[bc_a(ins)];
      if (getInfo(fnode)->type != FUN ||
          getFInfo(fnode)->code.arity != callargs)
        goto abort_recording;

      FuncInfoTable *info = getFInfo(fnode);
      u4 topslot = J->T->top - J->T->base;
      u4 framesize = info->code.framesize;
      u4 baseslot = J->T->base - J->startbase;
      const BCIns *return_pc = J->pc + BC_ROUND(callargs) + 2;
      int i;
      u1 *arg = (u1*)(J->pc + 1);

      if (LC_UNLIKELY(stackOverflow(J->T, J->T->top, 3 + framesize)))
        goto abort_recording;

      if (LC_UNLIKELY(J->T->base + topslot + 3 + framesize >= J->startbase + MAX_SLOTS)) {
        DBG_PR("ABORT: Frame too deep.");
        goto abort_recording;
      }

      ra = getSlot(J, bc_a(ins));  // The function

      setSlot(J, topslot + 0, emitKBaseOffset(J, baseslot));
      setSlot(J, topslot + 1, emitKWord(J, (Word)return_pc, LIT_PC));
      setSlot(J, topslot + 2, ra);
      for (i = 0; i < callargs; i++, arg++)
        setSlot(J, topslot + 3 + i, getSlot(J, *arg));

      DBG_PR("baseslot %d => %d (top = %d, frame = %d)\n",
             J->baseslot, J->baseslot + topslot + 3, topslot, framesize);
      J->baseslot += topslot + 3;
      J->base = J->slot + J->baseslot;
      J->maxslot = framesize;
      emit_raw(J, IRT(IR_FRAME, IRT_VOID), topslot + 3, framesize);
      for (i = callargs; i < J->maxslot; i++)
        setSlot(J, i, 0); // clear slots
      J->framedepth ++;
      //      fprintf(stderr, "exact CALL detected\n");
      //      LC_ASSERT(0);
    }
    break;

  case BC_UPDATE:
    {
      if (!(J->flags & JIT_OPT_CALL_BY_NAME)) {
        ra = getSlot(J, bc_a(ins));
        rb = getSlot(J, bc_d(ins));
        emit(J, IRT(IR_UPDATE, IRT_VOID), ra, rb);
      }
      break;
      /* J->last_result = rb; */
      /* J->needsnap = 1; */
      /* goto do_return; */
    }

  case BC_IRET:
  case BC_RET1:
    {
      /* Clear all slots but the result slot. */
      int i;
      for (i = 0; i < J->maxslot; i++) {
        if (i != bc_a(ins)) {
          clearSlot(J, i);
        }
      }
      J->last_result = getSlot(J, bc_a(ins));

      Word return_pc = tbase[-2];
      Word *return_base = (Word*)tbase[-3];
      i4 basediff = tbase - return_base;

      DBG_LVL(2, "Recording return: %d\n", basediff);

      IF_DBG_LVL(1, printSlots(J));

      J->framedepth--;
      guardEqualRetAddr(J, getSlot(J, -2), return_pc);

      /* Clear all slots and the frame of the return target. The
         result has been saved in J->last_result and will be loaded
         via BC_MOV_RES. */
      memset(&J->base[-3], 0, 3 * sizeof(TRef));

      if (J->framedepth < 0 && J->cur.traceType != RETURN_TRACE) {
        DBG_PR("ABORT: Returning outside of original frame (B).\n");
        goto abort_recording;
      } else {
        J->baseslot -= basediff;
      }

      if (J->minslot > J->baseslot - 3) {
        J->minslot = J->baseslot - 3;
      }

      // TODO: Do something with slot(-3)?
      DBG_LVL(2, "baseslot = %d\n", J->baseslot);
      //if (J->baseslot < INITIAL_BASE + 1) goto abort_recording;
      J->base = J->slot + J->baseslot;
      J->maxslot = basediff - 3;
      emit_raw(J, IRT(IR_RET, IRT_VOID), basediff, 0);

      IF_DBG_LVL(1, printSlots(J));

      //printSlots(J);
    }
    break;

  case BC_ALLOC1:
    {
      InfoTable *info = (InfoTable*)tbase[bc_b(ins)];
      TRef rinfo, rnew;
      rinfo = emitKWord(J, (Word)info, LIT_INFO);
      rb = getSlot(J, bc_b(ins));
      // Ensure that r(B) actually contains the the info table we're
      // expecting.  Usually, this will be optimised away.
      emit(J, IRT(IR_EQ, IRT_CMP), rb, rinfo);

      emit(J, IRT(IR_HEAPCHK, IRT_CMP), 2, 0);
      rc = getSlot(J, bc_c(ins));
      rnew = emit(J, IRT(IR_NEW, IRT_CLOS), rinfo, 2);
      setSlot(J, bc_a(ins), rnew);
      u4 h = newHeapInfo(J, rnew, info);
      IR(tref_ref(rnew))->op2 = h;
      setHeapInfoField(&J->cur, &J->cur.heap[h], 0, rc);
      //printSlots(J);
    }
    break;

  case BC_ALLOC:
    {
      InfoTable *info = (InfoTable*)tbase[bc_b(ins)];
      TRef rinfo, rnew;
      u4 size = bc_c(ins);
      u1 *arg = (u1*)(pc + 1);
      u4 i;
      rinfo = emitKWord(J, (Word)info, LIT_INFO);
      rb = getSlot(J, bc_b(ins));
      emit(J, IRT(IR_EQ, IRT_CMP), rb, rinfo);
      u4 h = newHeapInfo(J, 0, info);
      HeapInfo *hp = &J->cur.heap[h];
      for (i = 0; i < size; i++, arg++) {
        rc = getSlot(J, *arg);
        setHeapInfoField(&J->cur, hp, i, rc);
      }
      emit(J, IRT(IR_HEAPCHK, IRT_CMP), size + 1, 0);
      rnew = emit(J, IRT(IR_NEW, IRT_CLOS), rinfo, h);
      hp->ref = rnew;
      setSlot(J, bc_a(ins), rnew);
      //printSlots(J);
    }
    break;

  case BC_ALLOCAP:
    {
      u4 nargs = bc_c(ins);
      u4 pointer_mask = bc_b(ins);
      u4 h, i;
      TRef rinfo, rnew;
      InfoTable *info = getApInfoTable(nargs, pointer_mask);
      HeapInfo *hp;
      u1 *arg = (u1*)(pc + 1);

      emit(J, IRT(IR_HEAPCHK, IRT_CMP), nargs + 2, 0);

      rinfo = emitKWord(J, (Word)info, LIT_INFO);
      h = newHeapInfo(J, 0, info);
      // We currently assume that a NEW is never optimised away
      //IR(tref_ref(rnew))->op2 = h;
      hp = &J->cur.heap[h];
      for (i = 0; i < nargs + 1; i++, arg++) {
        rc = getSlot(J, *arg);
        setHeapInfoField(&J->cur, hp, i, rc);
      }
      //LC_ASSERT(J->cur.nins - 1 == tref_ref(rnew));
      rnew = emit(J, IRT(IR_NEW, IRT_CLOS), rinfo, h);
      hp->ref = rnew;
      setSlot(J, bc_a(ins), rnew);
    }
    break;

  case BC_CASE:
    {
      // The nice thing is, we don't care which case is selected, just
      // that the info table remains the same.
      ra = getSlot(J, bc_a(ins));
      Closure *cl = (Closure*)tbase[bc_a(ins)];
      ra = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
      guardEqualKWord(J, ra, (Word)getInfo(cl), LIT_INFO);
    }
    break;

  case BC_LOADSLF:
    {
      setSlot(J, bc_a(ins), getSlot(J, -1));
    }
    break;

  case BC_FUNC:
    break;

  case BC_IFUNC:
    // TODO: Should we abort tracing here?
    break;

  case BC_JFUNC:
  case BC_JRET:
    // TODO: Should we link traces here?
    goto abort_recording;
    break;

  case BC__MAX:
    /* We might be in recording mode when reaching the STOP instruction. */
    goto abort_recording;
    break;

  default:
    DBG_PR("record: Ignoring %s\n", ins_name[op]);
    LC_ASSERT(0);
    break;
  }

  IF_DBG_LVL(2, printSlots(J));

  if (J->cur.nins >= REF_BIAS + MAX_TRACE_LENGTH)
    goto abort_recording;

  return REC_CONT;

 abort_recording:
  recordCleanup(J);
  recordEvent(EV_ABORT_TRACE, 0);
  return REC_ABORT;
}

/* Default values for JIT parameters. */
static const int32_t jit_param_default[JIT_P__MAX+1] = {
#define JIT_PARAMINIT(len, name, value) (value),
JIT_PARAMDEF(JIT_PARAMINIT)
#undef JIT_PARAMINIT
  0
};

void
initJitState(JitState *J, const Opts* opts)
{
  J->mode = JIT_MODE_NORMAL;
  J->startpc = NULL;
  J->optimizations = JIT_OPT_DEFAULT;

  J->sizefragment = 256;
  //  J->maskfragment = J->sizefragment - 1;
  J->fragment = xmalloc(J->sizefragment * sizeof(*J->fragment));
  J->nfragments = 0;

  J->loghandle = (LC_DEBUG_LEVEL > 0) ? stderr : NULL;

  // Initialize jit parameters
  memcpy(J->param, jit_param_default, sizeof(J->param));
  J->param[JIT_P_enableasm] = opts->enable_asm;

  // Initialize exit stubs
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
}

LC_FASTCALL void
startRecording(JitState *J, BCIns *startpc, Thread *T, Word *base,
               TraceType type)
{
  T->base = base;
  J->startpc = startpc;
  J->startbase = base;
  J->cur.startpc = startpc;
  J->cur.traceType = type;
  J->mode = JIT_MODE_RECORDING;
  if (J->loghandle) {
    FuncInfoTable *info = getFInfo(base[-1]);
    LOG_JIT(J, "Starting trace at%s: %s (pc=%p)\n",
            type == RETURN_TRACE ? " return point in" : " ",
            info->name, startpc);
  }
  if (type == RETURN_TRACE) {
    DBG_LVL(2, "Starting a return trace");
  }
  recordSetup(J, T);
}

FragmentId
finishRecording(JitState *J, UnrollLevel unrollLevel)
{
  addSnapshot(J);
  bool unroll = (J->optimizations & JIT_OPT_UNROLL) && unrollLevel == UNROLL_ONCE;
  if (unroll) {
    J->cur.nloop = tref_ref(emit_raw(J, IRT(IR_LOOP, IRT_VOID), 0, 0));
  } else {
    emit_raw(J, IRT(IR_SAVE, IRT_VOID), J->cur.nsnap - 1, 0);
  }
  if (unroll) {
    // int i;
    optUnrollLoop(J);
  }

#ifndef NDEBUG
  printPrettyIR(&J->cur, J->nfragments);
  printIRBuffer(J);
  printHeapInfo(stderr, J);
#endif

  if (unroll) {
    optDeadCodeElim(J, PRE_ALLOC_SINK);
    if (J->optimizations & JIT_OPT_SINK_ALLOC) {
      heapSCCs(J);
    }
    fixHeapOffsets(J);
    optDeadCodeElim(J, POST_ALLOC_SINK);
    compactPhis(J);               /* useful for IR interpreter */
  } else {
    fixHeapOffsets(J);
    optDeadCodeElim(J, POST_ALLOC_SINK);
  }

  DBG_PR("*** Stopping to record.\n");

#ifndef NDEBUG
  printPrettyIR(&J->cur, J->nfragments);
  printIRBuffer(J);
  printHeapInfo(stderr, J);
#endif

  J->cur.orig = *J->startpc;

  if (J->cur.traceType == FUNCTION_TRACE) {
    *J->startpc = BCINS_AD(BC_JFUNC, 0, J->nfragments);
    DBG_PR("Overwriting startpc = %p, with: %x\n",
           J->startpc, *J->startpc);
  } else if (J->cur.traceType == RETURN_TRACE) {
    LC_ASSERT(bc_op(J->cur.orig) == BC_RET1);
    *J->startpc = BCINS_AD(BC_JRET, bc_a(J->cur.orig), J->nfragments);
    DBG_PR("Overwriting startpc = %p, with: %x\n",
           J->startpc, *J->startpc);
  }

/*
  if (unrollLevel != UNROLL_ONCE) {
    IRRef ref;
    for (ref = J->chain[IR_NEW]; ref >= REF_FIRST; ref = IR(ref)->prev) {
      HeapInfo *hpi = getHeapInfo(&J->cur, IR(ref));
      DBG_LVL(1, "is_sunken = %d, hp_offs = %d\n",
              ir_issunken(IR(ref)), hpi->hp_offs);
    }
    sayonara("Not yet implemented -- non-unrollable loops.");
  }
*/

#if LC_HAS_ASM_BACKEND
  if(J->param[JIT_P_enableasm]) {
    //TODO: compute the actual framsize of a trace
    //This number needs to be computed before a call to genAsm
    J->cur.framesize = J->framesize + 1;
    DBG_LVL(2, "Framesize %d\n", J->framesize);
    genAsm(J, &J->cur);
  }
  printIRBuffer(J);

#endif
  return registerCurrentFragment(J);
}



void
recordCleanup(JitState *J)
{
  xfree(J->irbuf + J->irmin);
  J->irbuf = J->cur.ir = NULL;
  J->irmin = J->irmax = 0;
  J->mode = JIT_MODE_NORMAL;

  xfree(J->snapbuf);
  xfree(J->snapmapbuf);
  J->snapbuf = J->cur.snap = NULL;
  J->sizesnap = 0;
  J->snapmapbuf = J->cur.snapmap = NULL;
  J->sizesnapmap = 0;

  xfree(J->heapbuf);
  xfree(J->heapmapbuf);
  J->heapbuf = J->cur.heap = NULL;
  J->heapmapbuf = J->cur.heapmap = NULL;
  J->sizeheap = 0;
  J->sizeheapmap = 0;

  xfree(J->kwordsbuf);
  J->kwordsbuf = J->cur.kwords = NULL;
  J->sizekwords = J->cur.nkwords = 0;
}

FragmentId
registerCurrentFragment(JitState *J)
{
  Fragment *F = xmalloc(sizeof(Fragment));

  F->startpc = J->cur.startpc;
  F->orig = J->cur.orig;

  F->nk = J->cur.nk;
  F->nins = J->cur.nins;  // TODO: remove NOPs
  F->nloop = J->cur.nloop;
  u4 szins = (F->nins - F->nk);
  F->ir = xmalloc(szins * sizeof(IRIns));
  F->nphis = J->cur.nphis;
  memcpy(F->ir, J->cur.ir + J->cur.nk, szins * sizeof(IRIns));
  F->ir -= F->nk;

  F->nkwords = J->cur.nkwords;
  F->kwords = xmalloc(F->nkwords * sizeof(Word));
  memcpy(F->kwords, J->cur.kwords, F->nkwords * sizeof(Word));

  F->nsnap = J->cur.nsnap;
  F->nsnapmap = J->cur.nsnapmap;
  F->snap = xmalloc(F->nsnap * sizeof(SnapShot));
  memcpy(F->snap, J->cur.snap, F->nsnap * sizeof(SnapShot));
  F->snapmap = xmalloc(F->nsnapmap * sizeof(SnapEntry));
  memcpy(F->snapmap, J->cur.snapmap, F->nsnapmap * sizeof(SnapEntry));

  F->nheap = J->cur.nheap;
  F->nheapmap = J->cur.nheapmap;
  F->heap = xmalloc(F->nheap * sizeof(HeapInfo));
  memcpy(F->heap, J->cur.heap, F->nheap * sizeof(HeapInfo));
  F->heapmap = xmalloc(F->nheapmap * sizeof(HeapEntry));
  memcpy(F->heapmap, J->cur.heapmap, F->nheapmap * sizeof(HeapEntry));

  F->framesize = J->cur.framesize;
  F->spills = J->cur.spills;
  F->mcode = J->cur.mcode;
  F->szmcode = J->cur.szmcode;

  recordCleanup(J);

  if (LC_UNLIKELY(J->nfragments >= J->sizefragment)) {
    fprintf(stderr, "FATAL: Too many fragments.\n");
    exit(1);
  }
  J->fragment[J->nfragments] = F;
  F->fragmentid = J->nfragments++;

#ifndef NDEBUG
  {
    FILE *traces = fopen("traces.pretty.txt", F->fragmentid == 0 ? "w" : "a");
    printPrettyIR_(traces, F, F->fragmentid);
    fclose(traces);

    if (F->mcode != NULL) {
      FILE *dump = fopen("traces.s", F->fragmentid == 0 ? "w" : "a");
      dumpAsm(F, dump);
      fclose(dump);
    }
  }
#endif

  return F->fragmentid;
}

#define SLOAD_INHERITED 1

static void
initSideTraceRecording(JitState *J, Fragment *parent, uint32_t exitno)
{
  SnapShot *snap = getSnapshot(parent, exitno);
  SnapEntry *p = getSnapshotEntries(parent, snap);
  int i, nent = snap->nent;
  J->framedepth = 0;
  DBG_LVL(2, "Initialising side trace from snapshot #%d\n", exitno);
  IF_DBG_LVL(2, printSnapshot(parent, snap, parent->snapmap));
  for (i = 0; i < nent; i++, p++) {
    int slot = (int)snap_slot(*p) - 1;
    IRRef ref = snap_ref(*p);
    IRIns *ir = IR(ref);
    TRef tr = 0;
    if (irref_islit(ref)) {
      switch (ir->o) {
      case IR_KINT:
        tr = emitKInt(J, ir->i);
        break;
      case IR_KWORD: {
        Word *kwords = parent->kwords;
        tr = emitKWord(J, kwords[ir->u], ir->t);
        break;
      }
      case IR_KBASEO:
        tr = emitKBaseOffset(J, ir->i);
        break;
      default:
        fprintf(stderr, "FATAL: Unknown literal type in "
                "initSideTraceRecording (%d).\n", ir->o);
        exit(2);
      }
    } else {
      /* TODO: handle sunken heap allocations. */
      i2 relslot = (i2)J->baseslot + slot - INITIAL_BASE;
      tr = emit_raw(J, IRT(IR_SLOAD, ir->t), relslot, SLOAD_INHERITED);
    }
    /* TODO: mark as written? */
    setSlot(J, slot, tr);
  }
  IF_DBG_LVL(2, printIRBuffer(J));
  IF_DBG_LVL(2, printSlots(J));
  fprintf(stderr, "NYI: recording side traces\n");
  exit(3);
}

LC_FASTCALL void
startRecordingSideTrace(JitState *J, Thread *T, Word *base,
                        Fragment *parent, uint32_t exitno)
{
  SnapShot *snap = getSnapshot(parent, exitno);
  BCIns *pc = getSnapshotPC(parent, snap);

  startRecording(J, pc, T, base, SIDE_TRACE);
  initSideTraceRecording(J, parent, exitno);
}

#undef IR
#undef foldIns

#endif /* LC_HAS_JIT */
