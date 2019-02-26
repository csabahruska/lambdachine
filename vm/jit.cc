#include "jit.hh"
#include "ir-inl.hh"
#include "assembler.hh"
#include "thread.hh"
#include "capability.hh"
#include "miscclosures.hh"
#include "time.hh"

#include <iostream>
#include <string.h>
#include <fstream>
#include <sstream>
#include <iomanip>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

_START_LAMBDACHINE_NAMESPACE

using namespace std;

uint64_t record_aborts = 0;
uint64_t record_abort_reasons[AR__MAX] = { 0, 0, 0, 0, 0 };

HotCounters::HotCounters(HotCount threshold)
  : threshold_(threshold) {
  for (Word i = 0; i < kNumCounters; ++i) {
    counters_[i] = threshold;
  }
}

Time jit_time = 0;

#if (DEBUG_COMPONENTS & DEBUG_TRACE_RECORDER) != 0
#define DBG(stmt) do { stmt; } while(0)
#else
#define DBG(stmt) do {} while(0)
#endif

FRAGMENT_MAP Jit::fragmentMap_;
std::vector<Fragment *> Jit::fragments_;

void Jit::resetFragments() {
  for (size_t i = 0; i < fragments_.size(); ++i) {
    delete fragments_[i];
  }
  fragments_.clear();
  fragmentMap_.clear();
}

uint32_t
Jit::numFragments()
{
  return (uint32_t)fragments_.size();
}

Jit::Jit()
  : cap_(NULL),
    startPc_(NULL), startBase_(NULL), parent_(NULL),
    flags_(), options_(), targets_(),
    prng_(), mcode_(&prng_), asm_(this) {
  Jit::resetFragments();
  memset(exitStubGroup_, 0, sizeof(exitStubGroup_));
  resetRecorderState();
#if (DEBUG_COMPONENTS & DEBUG_TRACE_PROGRESS)
  setDebugTrace(true);
#endif
#ifdef LC_TRACE_STATS
  stats_ = NULL;
#endif
}

Jit::~Jit() {
  Jit::resetFragments();
}

void Jit::beginRecording(Capability *cap, BcIns *startPc, Word *base, bool isReturn)
{
  LC_ASSERT(cap_ == NULL);
  LC_ASSERT(targets_.size() == 0);
  initRecording(cap, base, startPc);
  flags_.set(kIsReturnTrace, isReturn);
  traceType_ = TT_ROOT;
}

void Jit::setFallthroughParent(Fragment *parent, SnapNo snapno) {
  traceType_ = TT_FALLTHROUGH;
  parent_ = parent;
  buf_.parent_ = parent;
  parentExitNo_ = snapno;
}

void Jit::initRecording(Capability *cap, Word *base, BcIns *startPc)
{
  resetRecorderState();
  cap_ = cap;
  startPc_ = startPc;
  startBase_ = base;
  parent_ = NULL;
  parentExitNo_ = ~0;
  flags_.clear();
  buf_.reset(base, cap->currentThread()->top());
  callStack_.reset();
  btb_.reset(startPc_, &callStack_);
#ifdef LC_TRACE_STATS
  stats_ = NULL;
#endif
}

/*
  Find a possible duplicate of the refence in the snapshot.

  Searches through the snapshot up until `searchLimit`.  If it finds a
  duplicate returns the corresponding reference from `buf`s abstract
  stack. Otherwise, returns 0;
*/
static TRef
dedupSnapshotRef(IRBuffer *buf, Snapshot &snap, SnapshotData *snapmap,
                 SnapmapRef searchLimit, IRRef ref)
{
  SnapmapRef j;
  for (j = snap.begin(); j < searchLimit; ++j)
    if (snapmap->slotRef(j) == ref)
      return buf->slot(snapmap->slotId(j) - snap.relbase());
  return TRef();
}

void Jit::replaySnapshot(Fragment *parent, SnapNo snapno, Word *base)
{
  Snapshot &snap = parent->snap(snapno);
  SnapshotData *snapmap = &parent->snapmap_;
  int relbase = snap.relbase();
  BloomFilter seen = 0;
  uint32_t numInheritedSlots = 0;

  for (SnapmapRef i = snap.begin(); i < snap.end(); ++i) {
    int slot = snapmap->slotId(i) - relbase;
    IRRef ref = snapmap->slotRef(i);
    IR *ins = parent->ir(ref);
    TRef tref;

    // Check if we have seen this reference before.  Using a bloom
    // filter we avoid O(N^2) complexity.
    if (bloomtest(seen, ref)) { // We *may* have.
      tref = dedupSnapshotRef(&buf_, snap, snapmap, i, ref);
      if (tref != TRef())
        goto setslot;
    }
    bloomset(seen, ref);

    if (irref_islit(ref)) {
      // Offsets from the base pointer are relative to the parent
      // fragment's entry base.
      uint64_t k = parent->literalValue(ref, base - relbase);
      if (ins->opcode() == IR::kKBASEO) {
        tref = buf_.baseLiteral((Word *)k);
      } else {
        tref = buf_.literal(ins->type(), k);
      }
    } else {  // Not a literal.
      IRType ty = ins->type();
      tref = buf_.emitRaw(IRT(IR::kSLOAD, ty),
                          buf_.slots_.absolute(slot),
                          IR_SLOAD_INHERIT);
      uint16_t inherit_info;
      if (ins->spill() != 0) {
        inherit_info = RID_INIT | ((uint16_t)ins->spill() << 8);
      } else {
        inherit_info = (uint16_t)ins->reg();
      }
      buf_.parentmap_[numInheritedSlots++] = inherit_info;
    }
  setslot:
    buf_.setSlot(slot, tref);
  }

  buf_.stopins_ = REF_FIRST + numInheritedSlots;
  buf_.entry_relbase_ = relbase;
}

void Jit::beginSideTrace(Capability *cap, Word *base, Fragment *parent, SnapNo snapno) {
  LC_ASSERT(cap_ == NULL);
  LC_ASSERT(targets_.size() == 0);
  LC_ASSERT(cap != NULL);

  Snapshot &snap = parent->snap(snapno);
  initRecording(cap, base, snap.pc());

  traceType_ = TT_SIDE;
  parent_ = parent;
  parentExitNo_ = snapno;
  buf_.parent_ = parent;
  int parentHeapReserved = snap.overallocated(&parent->heap_);
  buf_.setParentHeapReserved(parentHeapReserved);

  replaySnapshot(parent, snapno, base);

  if (DEBUG_COMPONENTS & DEBUG_ASSEMBLER) {
    buf_.debugPrint(cerr, ~0);
    buf_.slots_.debugPrint(cerr);
  }
}

static IRType littypeToIRType(uint8_t littype) {
  // TODO: Check that this gets compiled to range check + lookup
  // table.
  switch (littype) {
  case LIT_INT:
    return IRT_I64;
  case LIT_CHAR:
    return IRT_CHR;
  case LIT_STRING:
    return IRT_PTR;
  case LIT_WORD:
    return IRT_U64;
  case LIT_CLOSURE:
    return IRT_CLOS;
  case LIT_INFO:
    return IRT_INFO;
  case LIT_PC:
    return IRT_PC;
  default:
    return IRT_UNKNOWN;
  }
}

static inline
uint8_t bcCond2irCond(uint8_t bc, bool invert) {
  return (IR::kLT + (bc - BcIns::kISLT)) ^ (uint8_t)(invert ? 1 : 0);
}

static bool evalCond(BcIns::Opcode opc, Word left, Word right) {
  switch (opc) {
  case BcIns::kISLT:
    return (WordInt)left < (WordInt)right;
  case BcIns::kISGE:
    return (WordInt)left >= (WordInt)right;
  case BcIns::kISLE:
    return (WordInt)left <= (WordInt)right;
  case BcIns::kISGT:
    return (WordInt)left > (WordInt)right;
  case BcIns::kISEQ:
    return (WordInt)left == (WordInt)right;
  case BcIns::kISNE:
    return (WordInt)left != (WordInt)right;
  case BcIns::kISLTU:
    return (Word)left < (Word)right;
  case BcIns::kISGEU:
    return (Word)left >= (Word)right;
  case BcIns::kISLEU:
    return (Word)left <= (Word)right;
  case BcIns::kISGTU:
    return (Word)left > (Word)right;
  default:
    cerr << "FATAL: (REC) Cannot evaluate condition: " << (int)opc;
    exit(2);
  }
}

static inline TRef
loadField(IRBuffer &buf_, TRef noderef, int offset, uint8_t type)
{
  TRef refref = buf_.emit(IR::kFREF, IRT_PTR, noderef, offset);
  return buf_.emit(IR::kFLOAD, type, refref, 0);
}

static inline TRef
specialiseOnInfoTable(IRBuffer &buf_, TRef noderef, Closure *node)
{
  InfoTable *info = node->info();
  TRef inforef = buf_.literal(IRT_INFO, (Word)info);
  buf_.emit(IR::kEQINFO, IRT_VOID | IRT_GUARD, noderef, inforef);
  return noderef;
}

static inline void
specialiseOnPapShape(IRBuffer &buf_, TRef papref, PapClosure *pap)
{
  // We need to have a guard for the expected size and bitmask of
  // the PAP since all PAPs share the same info table.
  TRef papinforef = loadField(buf_, papref, PAP_INFO_OFFSET / sizeof(Word),
                              IRT_I64);
  TRef papinfo_expected = buf_.literal(IRT_I64, pap->info_.combined);
  buf_.emit(IR::kEQ, IRT_VOID | IRT_GUARD, papinforef, papinfo_expected);
}

static inline Closure *
followIndirection(IRBuffer &buf_, int slot, Closure *tnode)
{
  TRef noderef = specialiseOnInfoTable(buf_, buf_.slot(slot), tnode);
  TRef newnoderef = loadField(buf_, noderef, 1, IRT_CLOS);
  buf_.setSlot(slot, newnoderef);
  return (Closure *)tnode->payload(0);
}

static inline void
clearSlots(IRBuffer &buf_, int start, int end)
{
  for (int i = start; i < end; ++i)
    buf_.setSlot(i, TRef());
}

static inline TRef
papOrDirectArg(IRBuffer &buf_, int arg, int pap_args, TRef *args, TRef pap_ref)
{
  if (arg >= pap_args)
    return args[arg - pap_args];
  else
    return loadField(buf_, pap_ref,
                     (PAP_PAYLOAD_OFFSET / sizeof(Word)) + arg,
                     IRT_UNKNOWN);
}

bool
Jit::recordGenericApply2(uint32_t call_info, Word *base,
                         TRef fnode_ref, Closure *fnode, TRef *args,
                         BcIns *returnPc)
{
  uint32_t direct_args = call_info & 0xff;
  uint32_t pointer_mask = call_info >> 8;
  PapClosure *pap = NULL;
  uint32_t pap_args = 0;

  // NOTE: The tricky bit here is that all the guards must occur
  // before any state changes.  If any of the guards fails, we need to
  // be exactly in the state we were when the CALL/CALLT happened.
  // There are many kinds of guards that may be necessary: info table
  // check, PAP shape check, heap check (for partial applications),
  // return address check (also partial applications).

  ClosureType type = fnode->info()->type();

  // Check if it's a PAP and emit the necessary guards.  Later parts
  // of the code then check whether there was a PAP later by comparing
  // `pap` to NULL.  Fortunately, the function part of a PAP can never
  // be another PAP (this is also needed for the GC).

  if (type == PAP) {
    pap = (PapClosure *)fnode;
    pap_args = pap->info_.nargs_;

    specialiseOnInfoTable(buf_, fnode_ref, fnode);
    specialiseOnPapShape(buf_, fnode_ref, pap);
    fnode = pap->fun_;
    type = fnode->info()->type();
    LC_ASSERT(type == FUN);

  } else if (type == THUNK || type == CAF) {

    specialiseOnInfoTable(buf_, fnode_ref, fnode);

    LC_ASSERT(pap == NULL);

    // Turn current frame into application continuation.
    BcIns *apk_return_addr = NULL;
    Closure *apk_closure = NULL;
    MiscClosures::getApCont(&apk_closure, &apk_return_addr,
                            direct_args, pointer_mask);
    u4 apk_framesize = MiscClosures::apContFrameSize(direct_args);
    TRef apk_closure_ref = buf_.literal(IRT_CLOS, (Word)apk_closure);

    // Create new frame (if CALL) or adjust size of current frame (if
    // CALLT).
    if (returnPc != NULL) { // CALL
      base = pushFrame(base, returnPc, apk_closure_ref, apk_framesize);
      if (!base) return false;
    } else {
      // Adjust size of current frame.
      if (!buf_.slots_.frame(base, base + apk_framesize)) {
        ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
        //        cerr << "Abstract stack overflow." << endl;
        return false;
      }
      buf_.setSlot(-1, apk_closure_ref);
    }

    // Put arguments into place.
    for (uint32_t i = 0; i < direct_args; ++i) {
      buf_.setSlot(i, args[i]);
    }
    clearSlots(buf_, direct_args, apk_framesize);

    TRef upd_clos_lit =
      buf_.literal(IRT_CLOS, (Word)MiscClosures::stg_UPD_closure_addr);
    Word *newbase = pushFrame(base, apk_return_addr, upd_clos_lit,
                              MiscClosures::UPD_frame_size);
    if (!newbase) return false;
    buf_.setSlot(0, fnode_ref);
    buf_.setSlot(1, TRef());

        // Push thunk frame
    const CodeInfoTable *info = (CodeInfoTable *)fnode->info();
    newbase = pushFrame(newbase, MiscClosures::stg_UPD_return_pc,
                        fnode_ref, info->code()->framesize);
    if (!newbase) return false;

    return true;
  }

  LC_ASSERT(type == FUN);

  uint32_t total_args = direct_args + pap_args;

  const CodeInfoTable *info = (CodeInfoTable *)fnode->info();
  uint32_t arity = info->code()->arity;

  if (arity == total_args) {  // Exact application.
    // We need a guard for the info table of the called function.
    // Again, this guard must occur before any slot writes.

    TRef funref = pap == NULL ? fnode_ref :
      loadField(buf_, fnode_ref, PAP_FUNCTION_OFFSET / sizeof(Word), IRT_CLOS);
    specialiseOnInfoTable(buf_, funref, fnode);

    uint32_t framesize = info->code()->framesize;
    if (returnPc) {
      base = pushFrame(base, returnPc, funref, framesize);
      if (!base) return false;
    } else {
      buf_.setSlot(-1, funref);
      if (!buf_.slots_.frame(base, base + framesize)) {
        ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
        return false;
      }
    }

    // Move arguments into place.
    if (pap) {
      for (uint32_t i = 0; i < pap_args; ++i) {
        buf_.setSlot(i, loadField(buf_, fnode_ref,
                                  (PAP_PAYLOAD_OFFSET / sizeof(Word)) + i,
                                  IRT_UNKNOWN));
      }
    }
    for (uint32_t i = 0; i < direct_args; ++i) {
      buf_.setSlot(pap_args + i, args[i]);
    }

    clearSlots(buf_, total_args, framesize);

    // buf_.debugPrint(cerr, -1);
    // buf_.slots_.debugPrint(cerr);
    // getchar();


    if (false && pap) {
      buf_.debugPrint(cerr, -1);
      buf_.slots_.debugPrint(cerr);
      cerr << "pap_payload_offset = " << (int)PAP_PAYLOAD_OFFSET
           << " pap_function_offset = " << (int)PAP_FUNCTION_OFFSET
           << " pap_info_offset = " << (int)PAP_INFO_OFFSET
           << " pap_info_size = " << (sizeof(pap->info_.combined))
           << " given args = " << direct_args
           << " pap args = " << pap_args
           << " total = " << total_args
           << " framesize = " << framesize
           << endl;
      cerr << " F: ";
      printClosure(cerr, pap->fun_, false);
      uint32_t mask = pap->info_.pointerMask_;
      for (int i = 0; i < pap_args; ++i, mask >>= 1) {
        cerr << " " << i << ": ";
        if (mask & 1) {
          Closure *cl = (Closure *)pap->payload(i);
          printClosure(cerr, cl, false);
        } else {
          cerr << hex << (Word)pap->payload(i) << dec << endl;
        }
      }
      //      getchar();
      // exit(1);
    }

    return true;

  } else if (arity < total_args) { // Overapplication.

    TRef funref = pap == NULL ? fnode_ref :
      loadField(buf_, fnode_ref, PAP_FUNCTION_OFFSET / sizeof(Word), IRT_CLOS);
    specialiseOnInfoTable(buf_, funref, fnode);

    uint32_t extra_args = total_args - arity;
    uint32_t apk_framesize = MiscClosures::apContFrameSize(extra_args);
    BcIns *apk_return_addr = NULL;
    Closure *apk_closure = NULL;
    MiscClosures::getApCont(&apk_closure, &apk_return_addr,
                            extra_args, pointer_mask >> arity);
    TRef apk_closure_ref = buf_.literal(IRT_CLOS, (Word)apk_closure);

    if (returnPc) { // CALL
      base = pushFrame(base, returnPc, apk_closure_ref, apk_framesize);
      if (!base) return false;
    } else {
      // Adjust size of current frame.
      if (!buf_.slots_.frame(base, base + apk_framesize)) {
        ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
        return false;
      }
      buf_.setSlot(-1, apk_closure_ref);
    }

    // Fill in application continuation frame.
    for (uint32_t i = 0; i < extra_args; ++i) {
      TRef ref = papOrDirectArg(buf_, arity + i, pap_args, args, fnode_ref);
      buf_.setSlot(i, ref);
    }

    clearSlots(buf_, extra_args, apk_framesize);

    uint32_t framesize = info->code()->framesize;
    Word *newbase = pushFrame(base, apk_return_addr, funref, framesize);
    if (!newbase) return false;

    for (uint32_t i = 0; i < arity; ++i) {
      buf_.setSlot(i, papOrDirectArg(buf_, i, pap_args, args, fnode_ref));
    }
    clearSlots(buf_, arity, framesize);

    if (pap) {
      buf_.debugPrint(cerr, -1);
      buf_.slots_.debugPrint(cerr);
      cerr << "pap_payload_offset = " << (int)PAP_PAYLOAD_OFFSET
           << " pap_function_offset = " << (int)PAP_FUNCTION_OFFSET
           << " given args = " << direct_args
           << " pap args = " << pap_args
           << endl;
      // uint32_t mask = pap->info_.pointerMask_;
      // for (int i = 0; i < pap_args; ++i, mask >>= 1) {
      //   cerr << " " << i << ": ";
      //   if (mask & 1) {
      //     cerr << ((Closure *)pap->payload(i))->info()->name
      //          << endl;
      //   } else {
      //     cerr << hex << (Word)pap->payload(i) << dec << endl;
      //   }
      // }
      exit(1);
    }

    return true;

  } else {
    TRef funref = pap == NULL ? fnode_ref :
      loadField(buf_, fnode_ref, PAP_FUNCTION_OFFSET / sizeof(Word), IRT_CLOS);
    specialiseOnInfoTable(buf_, funref, fnode);

    TRef expectedReturnPc;
    if (returnPc) {
      // We're going to immediately return to `returnPc`, so no guard
      // necessary.
    } else {
      // Otherwise, emit guard for the expected return address.
      TRef retref = buf_.slot(-2);
      expectedReturnPc = buf_.literal(IRT_PC, base[-2]);
      buf_.emit(IR::kEQ, IRT_VOID | IRT_GUARD, retref, expectedReturnPc);
    }

    uint32_t new_pap_size = wordsof(PapClosure) + total_args;
    buf_.emitHeapCheck(new_pap_size);
    // cerr << "new_pap_size = " << new_pap_size << endl;

    TRef new_pap_fields[66];
    // Make sure we have a reference for each PAP arg (if any) before
    // the NEW instruction.
    for (int i = 0; i < total_args; ++i) {
      new_pap_fields[(PAP_PAYLOAD_OFFSET / sizeof(Word)) - 1 + i] =
        papOrDirectArg(buf_, i, pap_args, args, fnode_ref);
    }

    if (pap) {
      pointer_mask <<= pap_args;
      pointer_mask |= pap->info_.pointerMask_;
    }
    PapInfo pap_info;
    pap_info.nargs_ = total_args;
    pap_info.pointerMask_ = pointer_mask;

    new_pap_fields[(PAP_INFO_OFFSET / sizeof(Word)) - 1] =
      buf_.literal(IRT_I64, pap_info.combined);
    new_pap_fields[(PAP_FUNCTION_OFFSET / sizeof(Word)) - 1] = funref;

    TRef pap_itbl = buf_.literal(IRT_INFO, (Word)MiscClosures::stg_PAP_info);

    IRBuffer::HeapEntry entry = 0;
    TRef new_pap = buf_.emitNEW(pap_itbl, new_pap_size - 1, &entry);
    for (int i = 0; i < new_pap_size - 1; ++i) {
      buf_.setField(entry, i, new_pap_fields[i]);
    }

    // buf_.debugPrint(cerr, -2);
    // buf_.slots_.debugPrint(cerr);
    // callStack_.debugPrint(cerr, &buf_, 0);

    if (returnPc) {
      // Set allocated as return result to be picked up by MOV_RES.
      buf_.setSlot(buf_.slots_.top() + FRAME_SIZE, new_pap);
    } else {
      // Return to parent, so we must clean up the current frame.
      clearSlots(buf_, -3, buf_.slots_.top());
      buf_.setSlot(0, new_pap);  // Set return result;
      LC_ASSERT(!expectedReturnPc.isNone());
      callStack_.returnTo(expectedReturnPc);
      Word *newbase = (Word *)base[-3];
      if (!buf_.slots_.frame(newbase, base - 3)) {
        ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
        return false;
      }
    }

    // buf_.slots_.debugPrint(cerr);
    // callStack_.debugPrint(cerr, &buf_, 0);
    //    getchar();

    return true;

    LC_ASSERT(arity > total_args);  // Partial application.
    logNYI(NYI_RECORD_CREATE_PAP);
    return false;
  }
}

/*
bool Jit::recordGenericApply(uint32_t call_info, Word *base,
                             TRef fnode_ref, Closure *fnode,
                             const Code *code) {
  uint32_t given_args = call_info & 0xff;
  uint32_t pointer_mask = call_info >> 8;

 generic_apply_retry:
  switch (fnode->info()->type()) {

  case PAP: {

    logNYI(NYI_RECORD_CALL_PAP);
    return false;

    PapClosure *pap = (PapClosure *)fnode;
    LC_ASSERT(pap->info()->type() == PAP);
    uint32_t papArgs = pap->info_.nargs_;

    // We need to have a guard for the expected size and bitmask of
    // the PAP since all PAPs share the same info table.
    TRef papinforef = loadField(buf_, fnode_ref, PAP_INFO_OFFSET / sizeof(Word),
                                IRT_I64);
    TRef papinfo_expected = buf_.literal(IRT_I64, pap->info_.combined);
    buf_.emit(IR::kEQ, IRT_VOID | IRT_GUARD, papinforef, papinfo_expected);

    uint32_t framesize = papArgs + given_args;
    if (!buf_.slots_.frame(base, base + framesize)) {
      cerr << "Abstract stack overflow." << endl;
      return false;
    }

    // Move up given arguments
    for (int i = given_args - 1; i >= 0; --i)
      buf_.setSlot(papArgs + i, buf_.slot(i));

    // Copy PAP args onto the stack.
    int offs = PAP_PAYLOAD_OFFSET / sizeof(Word);
    for (uint32_t i = 0; i < papArgs; ++i) {
      buf_.setSlot(i, loadField(buf_, fnode_ref, offs + i, IRT_UNKNOWN));
    }
    fnode = pap->fun_;
    buf_.setSlot(-1, loadField(buf_, fnode_ref, PAP_FUNCTION_OFFSET / sizeof(Word),
                               IRT_CLOS));
    specialiseOnInfoTable(buf_, buf_.slot(-1), fnode);

    buf_.debugPrint(cerr, -1);
    cerr << "pap_payload_offset = " << (int)PAP_PAYLOAD_OFFSET
         << " pap_function_offset = " << (int)PAP_FUNCTION_OFFSET
         << " given args = " << given_args
         << " pap args = " << papArgs
         << endl;
    //    exit(1);

    pointer_mask <<= papArgs;
    pointer_mask |= pap->info_.pointerMask_;
    given_args += papArgs;
    orig_framesize = 0;

    goto generic_apply_retry;
  }

  case THUNK:
  case CAF: {
    // Turn current frame into an App continuation.
    BcIns *apk_return_addr = NULL;
    Closure *apk_closure = NULL;
    MiscClosures::getApCont(&apk_closure, &apk_return_addr,
                            given_args, pointer_mask);
    uint32_t apk_framesize = MiscClosures::apContFrameSize(given_args);
    TRef apknoderef = buf_.literal(IRT_CLOS, (Word)apk_closure);
    buf_.setSlot(-1, apknoderef);
    int topslot = apk_framesize;
    // Adjust size of current frame.
    if (!buf_.slots_.frame(base, base + topslot)) {
      cerr << "Abstract stack overflow." << endl;
      return false;
    }

    // Push update frame.
    TRef upd_clos_lit =
      buf_.literal(IRT_CLOS, (Word)MiscClosures::stg_UPD_closure_addr);
    Word *newbase = pushFrame(base, apk_return_addr, upd_clos_lit,
                              MiscClosures::UPD_frame_size);
    if (!newbase) return false;
    buf_.setSlot(0, fnode_ref);
    buf_.setSlot(1, TRef());

    // Push thunk frame
    const CodeInfoTable *info = (CodeInfoTable *)fnode->info();
    newbase = pushFrame(newbase, MiscClosures::stg_UPD_return_pc,
                        fnode_ref, info->code()->framesize);
    if (!newbase) return false;

    return true;
  }
    //logNYI(NYI_RECORD_CALL_THUNK);
    //return false;
  case FUN: {
    const CodeInfoTable *info = (CodeInfoTable *)fnode->info();
    uint32_t arity = info->code()->arity;
    if (arity == given_args) {


      return true;

    }
    if (arity > given_args) {
      logNYI(NYI_RECORD_CREATE_PAP);
      return false;
    }

    // Overapplication.
    DBG(cerr << "REC: overapplication" << endl);

    TRef scratch[BcIns::kMaxCallArgs];
    for (int i = 0; i < given_args; ++i) {
      scratch[i] = buf_.slot(i);
    }
    uint32_t extra_args = given_args - arity;

    BcIns *apk_return_addr = NULL;
    Closure *apk_closure = NULL;
    MiscClosures::getApCont(&apk_closure, &apk_return_addr,
                            extra_args, pointer_mask >> arity);
    uint32_t apk_frame_size = MiscClosures::apContFrameSize(extra_args);

    // 1. Turn current frame into App continuation.
    buf_.setSlot(-1, buf_.literal(IRT_CLOS, (Word)apk_closure));
    for (int i = 0; i < extra_args; ++i) {
      buf_.setSlot(i, scratch[arity + i]);
    }
    buf_.setSlot(extra_args, TRef());

    buf_.slots_.frame(base, base + apk_frame_size);


    // 2. Build new frame for actual application.
    uint32_t framesize = info->code()->framesize;
    Word *newbase = pushFrame(base, apk_return_addr, fnode_ref, framesize);
    if (!newbase)
      return false;
    for (int i = 0; i < arity; ++i) {
      buf_.setSlot(i, scratch[i]);
    }

    return true;
  }
  default:
    cerr << "ERROR: Recorded call to non-FUN/PAP/THUNK." << endl;
    return false;
  }
}
*/

bool Jit::recordIns(BcIns *ins, Word *base, const Code *code) {
  try {

  if (LC_UNLIKELY(shouldAbort_)) {
    ++record_abort_reasons[AR_INTERPRETER_REQUEST];
    goto abort_recording;
  }
  buf_.pc_ = ins;
  buf_.steps_++;
  DBG(cerr << "REC: " << ins << " " << ins->name() << endl);

  if (flags_.get(kLastInsWasBranch)) {

    // Check if we found a loop.
    DBG(callStack_.debugPrint(cerr, &buf_, STACK_NO_REF));
    DBG(cerr << "Entry "; callStack_.debugPrint(cerr, &buf_, 0));

    int loopentry = -1;
    if (ins != MiscClosures::stg_UPD_return_pc)  // HACK!
      loopentry = btb_.isTrueLoop(ins);
    if (LC_LIKELY(loopentry == -1)) {  // Not a loop.
      btb_.emit(ins);
      if (btb_.size() > 100) {
        ++record_abort_reasons[AR_TRACE_TOO_LONG];
        // cerr << COL_RED << "TRACE TOO LONG (" << btb_.size()
        //      << ")" << COL_RESET << endl;
        goto abort_recording;
      }
    } else {  // We found a true loop.
      if (loopentry == 0) {
        DBG(cerr << "REC: Loop to entry detected." << endl);
        buf_.emit(IR::kSAVE, IRT_VOID | IRT_GUARD, IR_SAVE_LOOP, 0);
        finishRecording();
        return true;

      } else if (ins->opcode() != BcIns::kIFUNC) {
        // We found an inner loop.  We'd really want the loop to be
        // its own trace.  So we cut off the current trace and
        // directly fall back to the interpreter.  A new trace will
        // quickly form at the fall-back point, which will then
        // (hopefully) form a proper loop.
        //
        // Ideally, we'd like to cut off the trace *before* the inner
        // loop, but doing it this way is easier to implement, ATM.

        // We simply continue recording if the loop is caused by an
        // IFUNC, which usually indicates that it's an AP continuation
        // or an AP thunk, i.e., some kind of generic code.
        DBG(cerr << COL_GREEN << "REC: Inner loop. " << buf_.pc_ << COL_RESET << endl);
        buf_.emit(IR::kSAVE, IRT_VOID | IRT_GUARD, IR_SAVE_FALLTHROUGH, 0);
        finishRecording();
        return true;
      }
    }
  }

  // This is overwritten by branch instructions.
  flags_.clear(kLastInsWasBranch);

  LC_ASSERT(cap_ != NULL);

  switch (ins->opcode()) {
  case BcIns::kIFUNC:
  case BcIns::kFUNC:
    buf_.slots_.frame(base, base + ins->a());
    break;
  case BcIns::kLOADK: {
    u2 lit_id = ins->d();
    Word lit = code->lits[lit_id];
    IRType ty = littypeToIRType(code->littypes[lit_id]);
    TRef litref = buf_.literal(ty, lit);
    buf_.setSlot(ins->a(), litref);
    break;
  }
  case BcIns::kISGT:
  case BcIns::kISLT:
  case BcIns::kISGE:
  case BcIns::kISLE:
  case BcIns::kISGTU:
  case BcIns::kISLTU:
  case BcIns::kISGEU:
  case BcIns::kISLEU:
  case BcIns::kISEQ:
  case BcIns::kISNE: {
    bool taken = evalCond(ins->opcode(), base[ins->a()], base[ins->d()]);
    TRef aref = buf_.slot(ins->a());
    TRef bref = buf_.slot(ins->d());
    uint8_t iropc = bcCond2irCond(ins->opcode(), !taken);
    buf_.emit(iropc, IRT_VOID | IRT_GUARD, aref, bref);
    // These branches cannot trigger a new trace to start.
    //    flags_.set(kLastInsWasBranch);
    break;
  }

#define ARITH_OP_RRR(bcop, irop, irtype) \
  case BcIns::bcop: { \
    TRef bref = buf_.slot(ins->b()); \
    TRef cref = buf_.slot(ins->c()); \
    TRef aref = buf_.emit(IR::irop, irtype, bref, cref); \
    buf_.setSlot(ins->a(), aref); \
    break; \
  }

#define ARITH_OP_RR(bcop, irop, irtype) \
  case BcIns::bcop: { \
    TRef dref = buf_.slot(ins->d()); \
    TRef aref = buf_.emit(IR::irop, irtype, dref, TRef()); \
    buf_.setSlot(ins->a(), aref); \
    break; \
  }

    ARITH_OP_RRR(kADDRR, kADD, IRT_I64);
    ARITH_OP_RRR(kSUBRR, kSUB, IRT_I64);
    ARITH_OP_RRR(kMULRR, kMUL, IRT_I64);
    ARITH_OP_RRR(kREMRR, kREM, IRT_I64);
    ARITH_OP_RRR(kDIVRR, kDIV, IRT_I64);
    ARITH_OP_RR(kNEG, kNEG, IRT_I64);

    ARITH_OP_RR(kBNOT, kBNOT, IRT_I64);
    ARITH_OP_RRR(kBAND, kBAND, IRT_I64);
    ARITH_OP_RRR(kBXOR, kBXOR, IRT_I64);
    ARITH_OP_RRR(kBOR, kBOR, IRT_I64);

    ARITH_OP_RRR(kBSHL, kBSHL, IRT_I64);
    ARITH_OP_RRR(kBSHR, kBSHR, IRT_I64);

#undef ARITH_OP_RR
#undef ARITH_OP_RRR

  case BcIns::kPTROFSC: {
    TRef ptrref = buf_.slot(ins->b());
    TRef ofsref = buf_.slot(ins->c());
    // TODO: At some point in the future we might want to split this
    // into a xREF and a load to make CSE on pointer references
    // easier.  We also currently assume that the pointed-to data is
    // constant (as is the case for string literals).
    TRef aref = buf_.emit(IR::kPLOAD, IRT_I64, ptrref, ofsref);
    buf_.setSlot(ins->a(), aref);
    break;
  }
  case BcIns::kCALLT: {
    // TODO: Detect and optimise recursive calls into trace specially?
    Closure *clos = (Closure *)base[ins->a()];
    uint32_t direct_args = ins->c();

    while (clos->isIndirection()) {
      clos = followIndirection(buf_, ins->a(), clos);
    }

    TRef args[32];
    LC_ASSERT(direct_args <= 32);
    for (int i = 0; i < direct_args; ++i)
      args[i] = buf_.slot(i);

    TRef node_ref = buf_.slot(ins->a());
    uint32_t ptr_mask = *(uint32_t *)(ins + 1);

    uint32_t call_info = (uint32_t)ins->c() | (ptr_mask << 8);
    if (!recordGenericApply2(call_info, base, node_ref, clos, args, NULL))
      goto abort_recording;

    /*
    TRef fnode = specialiseOnInfoTable(buf_, buf_.slot(ins->a()), clos);

    // Clear all non-argument registers.
    for (int i = ins->c(); i < code->framesize; ++i) {
      buf_.setSlot(i, TRef());
    }
    buf_.setSlot(-1, fnode);  // TODO: Can't write to slot before the guard?

    //    buf_.slots_.debugPrint(cerr);
        if (!recordGenericApply(call_info, base, fnode, clos, code, orig_framesize))
      goto abort_recording;
    //    buf_.slots_.debugPrint(cerr);
    */

    flags_.set(kLastInsWasBranch);
    break;
  }

  case BcIns::kCALL: {
    Closure *clos = (Closure *)base[ins->a()];
    uint32_t nargs = ins->c();

    while (clos->isIndirection()) {
      clos = followIndirection(buf_, ins->a(), clos);
    }

    TRef fnode_ref = buf_.slot(ins->a());

    TRef args[32];
    LC_ASSERT(nargs < 32);
    uint32_t ptr_mask = *(uint32_t *)(ins + 1);
    ++ins;

    uint8_t *arg = (uint8_t *)(ins + 1);
    for (int i = 0; i < nargs; ++i, ++arg) {
      args[i] = buf_.slot(*arg);
    }

    // See interpreter implementation for details.
    BcIns *returnPc = ins + 1 + BC_ROUND(nargs) + 1;

    uint32_t call_info = (uint32_t)nargs | (ptr_mask << 8);
    if (!recordGenericApply2(call_info, base, fnode_ref, clos, args, returnPc))
      goto abort_recording;

    flags_.set(kLastInsWasBranch);
    break;

    /*
    TRef fnode = specialiseOnInfoTable(buf_, buf_.slot(ins->a()), clos);
    InfoTable *info = clos->info();

    const Code *code = ((FuncInfoTable *)info)->code();
    TRef argref[32];
    LC_ASSERT(nargs <= 32);

    uint8_t *arg = (uint8_t *)(ins + 1);

    for (int i = 0; i < nargs; ++i, ++arg) {
      argref[i] = buf_.slot(*arg);
    }

    // See interpreter implementation for details.
    BcIns *returnPc = ins + 1 + BC_ROUND(nargs) + 1;

    Word *newbase = pushFrame(base, returnPc, fnode, code->framesize);
    if (!newbase) goto abort_recording;

    for (int i = 0; i < nargs; ++i) {
      buf_.setSlot(i, argref[i]);
    }

    uint32_t call_info = (uint32_t)nargs | ((uint32_t)ins->b() << 8);
    if (!recordGenericApply(call_info, newbase, fnode, clos, code))
      goto abort_recording;

    flags_.set(kLastInsWasBranch);
    break;
    */
  }

  case BcIns::kMOV:
    buf_.setSlot(ins->a(), buf_.slot(ins->d()));
    break;

  case BcIns::kLOADSLF:
    buf_.setSlot(ins->a(), buf_.slot(-1));
    break;

  case BcIns::kEVAL: {
    Closure *tnode = (Closure *)base[ins->a()];
    while (tnode->isIndirection()) {
      tnode = followIndirection(buf_, ins->a(), tnode);
    }
    TRef noderef = buf_.slot(ins->a());
    TRef inforef = buf_.literal(IRT_INFO, (Word)tnode->info());
    buf_.emit(IR::kEQINFO, IRT_VOID | IRT_GUARD, noderef, inforef);
    if (tnode->isHNF()) {
      Word *top = cap_->currentThread()->top();
      int topslot = top - base;
      buf_.setSlot(topslot + FRAME_SIZE, noderef);
      // TODO: Clear dead registers.
    } else {
      Word *top = cap_->currentThread()->top();
      int topslot = top - base;
      LC_ASSERT(buf_.slots_.top() == topslot);
      CodeInfoTable *info = static_cast<CodeInfoTable *>(tnode->info());
      u4 framesize = info->code()->framesize;
      // TODO: Check for stack overflow and abort recording?
      BcIns *returnPc = ins + 2;

      //      buf_.slots_.debugPrint(cerr);

      TRef upd_clos_lit =
        buf_.literal(IRT_CLOS, (Word)MiscClosures::stg_UPD_closure_addr);
      Word *newbase = pushFrame(base, returnPc, upd_clos_lit,
                                MiscClosures::UPD_frame_size);
      if (!newbase) goto abort_recording;

      buf_.setSlot(0, noderef);
      buf_.setSlot(1, TRef());

      newbase = pushFrame(newbase, MiscClosures::stg_UPD_return_pc,
                          noderef, framesize);
      if (!newbase) goto abort_recording;

      // Clear all slots, just to be safe.
      for (int i = 0; i < framesize; ++i) {
        buf_.setSlot(i, TRef());
      }

      //      buf_.slots_.debugPrint(cerr);
      flags_.set(kLastInsWasBranch);

    }
    break;
  }

  case BcIns::kIRET:
  case BcIns::kRET1: {
    //    buf_.slots_.debugPrint(cerr);

    TRef retref = buf_.slot(-2);
    TRef expectedReturnPc = buf_.literal(IRT_PC, base[-2]);
    buf_.emit(IR::kEQ, IRT_VOID | IRT_GUARD, retref, expectedReturnPc);

    callStack_.returnTo(expectedReturnPc);

    TRef resultref = buf_.slot(ins->a());

    // Clear current frame.
    for (int i = -3; i < (int)buf_.slots_.top(); ++i) {
      buf_.setSlot(i, TRef());
    }
    // Put return result back into frame.
    buf_.setSlot(0, resultref);

    // Return address implies framesize, thus we don't need an extra
    // guard.  In fact, storing all these frame pointers on the stack
    // is quite wasteful.
    Word *newbase = (Word *)base[-3];
    if (!buf_.slots_.frame(newbase, base - 3)) {
      ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
      goto abort_recording;
    }

    //    buf_.slots_.debugPrint(cerr);
    flags_.set(kLastInsWasBranch);
    break;
  }

  case BcIns::kRETN: {
    TRef retref = buf_.slot(-2);
    TRef expectedReturnPc = buf_.literal(IRT_PC, base[-2]);
    buf_.emit(IR::kEQ, IRT_VOID | IRT_GUARD, retref, expectedReturnPc);

    callStack_.returnTo(expectedReturnPc);

    // Clear current frame, except for the return results.
    for (int i = -3; i < 0; ++i) {
      buf_.setSlot(i, TRef());
    }

    for (int i = ins->a(); i < (int)buf_.slots_.top(); ++i) {
      buf_.setSlot(i, TRef());
    }

    // Return address implies framesize, thus we don't need an extra
    // guard.  In fact, storing all these frame pointers on the stack
    // is quite wasteful.
    Word *newbase = (Word *)base[-3];
    if (!buf_.slots_.frame(newbase, base - 3)) {
      ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
      // cerr << "Abstract stack overflow/underflow" << endl;
      goto abort_recording;
    }

    flags_.set(kLastInsWasBranch);
    break;
  }

  case BcIns::kMOV_RES: {
    // NOTE: We rely on the top pointer to be correct.
    int topslot = buf_.slots_.top();
    int resultslot = topslot + FRAME_SIZE + ins->d();
    TRef result = buf_.slot(resultslot);
    // Clear old value to avoid unnecessarily creating a snapshot
    // entry for it.  The bytecode compiler never emits a MOV_RES
    // twice for the same input.
    buf_.setSlot(resultslot, TRef());
    buf_.setSlot(ins->a(), result);
    break;
  }

  case BcIns::kUPDATE: {
    Closure *oldnode = (Closure *)base[ins->a()];
    InfoTable *info = oldnode->info();

    if (info->type() == CAF) {
      logNYI(NYI_RECORD_UPDATE_CAF);
      ++record_abort_reasons[AR_NYI];
      goto abort_recording;
    }

    TRef oldref = buf_.slot(ins->a());
    TRef newref = buf_.slot(ins->d());

    // TODO: Update behaves differently for CAFs and for Thunks. CAFs
    // need to be added to the static references table. For now, we
    // overspecialise, by emitting a guard on the info table. An
    // alternative could be to do the additional work by triggering a
    // write barrier which is needed for generational/incremental GC,
    // anyway.
    //
    // Note: In most cases, this guard should be redundant because the
    // update was executed because we evaluated a thunk on the trace.
    // If we change the code of thunks to do the update themselves
    // we can also use two variants of UPDATE, one for CAFs and one for
    // regular thunks.
    TRef inforef = buf_.literal(IRT_INFO, (Word)info);
    buf_.emit(IR::kEQINFO, IRT_VOID | IRT_GUARD, oldref, inforef);

    buf_.emit(IR::kUPDATE, IRT_VOID, oldref, newref);
    break;
  }

  case BcIns::kLOADF: {
    TRef rbase = buf_.slot(ins->b());
    TRef fref = buf_.emit(IR::kFREF, IRT_PTR, rbase, ins->c());
    TRef res = buf_.emit(IR::kFLOAD, IRT_UNKNOWN, fref, 0);
    buf_.setSlot(ins->a(), res);
    break;
  }

  case BcIns::kLOADFV: {
    TRef rbase = buf_.slot(-1);
    TRef fref = buf_.emit(IR::kFREF, IRT_PTR, rbase, ins->d());
    TRef res = buf_.emit(IR::kFLOAD, IRT_UNKNOWN, fref, 0);
    buf_.setSlot(ins->a(), res);
    break;
  }

  case BcIns::kALLOC1: {
    TRef itbl = buf_.slot(ins->b());
    TRef field = buf_.slot(ins->c());
    buf_.emitHeapCheck(2);
    IRBuffer::HeapEntry entry = 0;
    TRef clos = buf_.emitNEW(itbl, 1, &entry);
    buf_.setField(entry, 0, field);
    buf_.setSlot(ins->a(), clos);
    break;
  }

  case BcIns::kALLOC: {
    TRef itbl = buf_.slot(ins->b());
    int nfields = ins->c();
    buf_.emitHeapCheck(1 + nfields);
    IRBuffer::HeapEntry entry = 0;
    const uint8_t *args = (const uint8_t *)(ins + 1);

    // Make sure we have a ref for each field before emitting the NEW.
    // This may emit SLOAD instructions, so make sure those occur
    // before the NEW.
    for (int i = 0; i < nfields; ++i) buf_.slot(*args++);

    TRef clos = buf_.emitNEW(itbl, nfields, &entry);
    args = (const uint8_t *)(ins + 1);
    for (int i = 0; i < nfields; ++i) {
      TRef field = buf_.slot(*args++);
      buf_.setField(entry, i, field);
    }
    buf_.setSlot(ins->a(), clos);
    break;
  }

  case BcIns::kALLOCAP: {
    uint32_t nfields = ins->c() + 1;
    uint32_t pointer_mask = ins->b();
    const uint8_t *args = (const uint8_t *)(ins + 1);

    buf_.emitHeapCheck(1 + nfields);

    // Make sure we have a ref for each field before emitting the NEW.
    // This may emit SLOAD instructions, so make sure those occur
    // before the NEW.
    for (int i = 0; i < nfields; ++i)
      buf_.slot(*args++);

    InfoTable *info = MiscClosures::getApInfo(ins->c(), pointer_mask);
    TRef itbl = buf_.literal(IRT_INFO, (Word)info);
    IRBuffer::HeapEntry entry = 0;
    TRef clos = buf_.emitNEW(itbl, nfields, &entry);
    args = (const uint8_t *)(ins + 1);
    for (int i = 0; i < nfields; ++i) {
      TRef field = buf_.slot(*args++);
      buf_.setField(entry, i, field);
    }
    buf_.setSlot(ins->a(), clos);
    break;
  }

  case BcIns::kCASE_S:
    // TODO: It is quite common to have only one alternative for
    // sparse cases.  In that case we really have just a binary branch
    // and it would be easier to generate a NEINFO guard.  I.e., we
    // only care whether it's the case mentioned in the CASE_S or any
    // other.  Unfortunately, that requires a mechanism to get an info
    // table from a tag, which we don't have yet.
  case BcIns::kCASE: {
    Closure *cl = (Closure *)base[ins->a()];
    TRef clos = buf_.slot(ins->a());
    TRef itbl = buf_.literal(IRT_INFO, (Word)cl->info());
    buf_.emit(IR::kEQINFO, IRT_VOID | IRT_GUARD, clos, itbl);
    break;
  }

  case BcIns::kJMP:
    // Nothing to do here.
    break;

  case BcIns::kJFUNC: {
    Fragment *parent = parent_;
    Fragment *F = lookupFragment(ins);
    while (parent) {
      if (F == parent) {
#if (DEBUG_COMPONENTS & DEBUG_TRACE_RECORDER)
        cerr << COL_RED "Loop-back to parent ("
             << parent->traceId() << ") found." COL_RESET "\n";
#endif
        buf_.emit(IR::kSAVE, IRT_VOID | IRT_GUARD, IR_SAVE_LINK,
                  parent->traceId());
        finishRecording();
        return true;
      }
      parent = parent->parent_;
    }
    if (!parent) {
      // For now we just use "stop-at-existing trace".
      LC_ASSERT(F->startPc() == ins);
      DBG(cerr << "Linking to existing trace: " << F->traceId() << endl);
      buf_.emit(IR::kSAVE, IRT_VOID | IRT_GUARD, IR_SAVE_LINK,
                F->traceId());
      finishRecording();
      return true;
    }
  }

  case BcIns::kGETTAG: {
    // WARNING: We currently overspecialise.  The idea is that GETTAG
    // is usually followed by an integer comparison on the tag.  So we
    // can specialise on the info-table and just load a static
    // constant.  This may not be a good idea in other cases.
    Closure *cl = (Closure *)base[ins->d()];
    LC_ASSERT(!cl->isIndirection() && cl->isHNF());
    specialiseOnInfoTable(buf_, buf_.slot(ins->d()), cl);
    TRef taglit = buf_.literal(IRT_I64, cl->tag() - 1);
    //    cerr << "nodespec = " << buf_.slot(ins->d()).ref() - REF_BIAS << endl;
    buf_.setSlot(ins->a(), taglit);
    break;
  }

  default:
    cerr << "NYI: Recording of " << ins->name() << endl;
    goto abort_recording;
  }

  //  buf_.slots_.debugPrint(cerr);
  // buf_.debugPrint(cerr, 0);

  return false;

abort_recording:
  ++record_aborts;
  resetRecorderState();
  return true;


  } catch (int err) {
    switch (err) {
    case IROPTERR_FAILING_GUARD:
      DBG(cerr << "Aborting due to permanently failing guard.\n");
      ++record_aborts;
      ++record_abort_reasons[AR_KNOWN_TO_FAIL_GUARD];
      resetRecorderState();
      return true;
    default:
      cerr << "Unknown error condition.\n";
      throw err;
    }
  }
}

inline void Jit::resetRecorderState() {
  flags_.clear();
  targets_.clear();
  cap_ = NULL;
  shouldAbort_ = false;
}

void Jit::finishRecording() {
  Time compilestart = getProcessElapsedTime();
  DBG(cerr << "Recorded: " << endl);
#ifdef LC_TRACE_STATS
  uint32_t nStatCounters = 1 + buffer()->snaps_.size();
  stats_ = new uint64_t[nStatCounters];
  memset(stats_, 0, sizeof(uint64_t) * nStatCounters);
#endif
  asm_.assemble(buffer(), mcode());
  if (DEBUG_COMPONENTS & DEBUG_ASSEMBLER)
    buf_.debugPrint(cerr, Jit::numFragments());

  int tno = fragments_.size();

  Fragment *F = saveFragment();

  registerFragment(startPc_, F, traceType_ == TT_SIDE);

  if (parent_ != NULL) {
    asm_.patchGuard(parent_, parentExitNo_, F->entry());
  }

  if (traceType_ != TT_SIDE && !flags_.get(kIsReturnTrace)) {
#if (DEBUG_COMPONENTS & DEBUG_TRACE_RECORDER)
    cerr << "Writing JFUNC (isReturn=" << flags_.get(kIsReturnTrace) << ")\n";
#endif
    *startPc_ = BcIns::ad(BcIns::kJFUNC, 0, tno);
  }

#ifdef LC_CLEAR_DOM_COUNTERS
  // See Note "Reset Dominated Counters" below.
  btb_.resetDominatedCounters(cap_);
#endif

#ifdef LC_DUMP_TRACES
  {
    ofstream out;
    out.open("dump_traces.txt",
             (tno == 0 ? ofstream::trunc : ofstream::app) | ofstream::out);
    buf_.debugPrint(out, tno);
    out.close();
  }
#endif

  resetRecorderState();

  if (DEBUG_COMPONENTS & DEBUG_ASSEMBLER) {
    ofstream out;
    stringstream filename;
    filename << "dump_Trace_" << (int)tno << ".s";
    out.open(filename.str().c_str());
    mcode()->dumpAsm(out);
    out.close();
  };

  jit_time += getProcessElapsedTime() - compilestart;
}

void
Jit::patchFallthrough(Fragment *parent, ExitNo exitno, Fragment *target)
{
  asm_.patchFallthrough(parent, exitno, target);
}

/*

Note: Reset Dominated Conters
-----------------------------

If the recorded trace contained several possible trace roots they most
likely all have similar hot counters.  Let's call the trace we've just
compiled T and E1 its entry point (startPc).  Assume there is another
potential trace head E2 inside T.  Since E1 reached the hotness
threshold it is quite likely that E2 is very close to the hotness
threshold as well, most likely it is only one tick away from the
hotness threshold.

Let's now consider the case that T executes and exits.  It is quite
likely that E2 is reached again and a new trace T2 is created.  That
is problematic.  If the same execution path is taken more frequently
we will eventually create a side trace that also reaches E2.  There
are now two possibilities:

  - if we stop recording at an existing trace we link with T2.  This
    may mean loss of optimisation potential.

  - if we keep recording despite existing traces, then we create a
    new trace that includes T2.  The trace T2 is now essentially
    dead code.

We thus reset all counters for potential trace heads inside T to
increase the likelyhood that a side trace of T is created before T2 is
created.  It is not a 100% solution, but it shouldn't hurt either.

*/

Fragment::Fragment()
  : flags_(0), traceId_(0), startPc_(NULL), targets_(NULL) {
#ifdef LC_TRACE_STATS
  stats_ = NULL;
#endif
}

Fragment::~Fragment() {
  if (targets_ != NULL)
    delete[] targets_;
#ifdef LC_TRACE_STATS
  if (stats_ != NULL)
    delete[] stats_;
#endif
}

void Jit::genCode(IRBuffer *buf) {
  IRRef ref;
  for (ref = buf->bufmax_; ref > REF_BASE; --ref) {
    IR *tir = buf->ir(ref);
    genCode(buf, tir);
  }
}

Fragment *Jit::saveFragment() {
  IRBuffer *buf = &buf_;
  Assembler *as = &asm_;

  Fragment *F = new Fragment();
  F->traceId_ = fragments_.size();
  F->startPc_ = startPc_;
  F->parent_ = parent_;

  F->numTargets_ = targets_.size();
  F->targets_ = new BcIns*[F->numTargets_];
  for (size_t i = 0; i < F->numTargets_; ++i)
    F->targets_[i] = targets_.at(i);

  long bufsize = (long)buf->bufmax_ - (long)buf->bufmin_;
  IR *buffer = new IR[bufsize];
  F->firstconstant_ = buf->bufmin_;
  F->nextins_ = buf->bufmax_;
  buffer = biasBuffer(buffer, -(F->firstconstant_ - REF_BIAS));
  for (IRRef ref = F->firstconstant_; ref < F->nextins_; ++ref)
    buffer[ref] = buf->buffer_[ref]; // TODO: use memcpy
  F->buffer_ = buffer;

  LC_ASSERT(buf->slots_.highestSlot() >= 0);
  F->frameSize_ = buf->slots_.highestSlot();

  size_t nsnaps = buf->snaps_.size();
  F->nsnaps_ = nsnaps;
  F->snaps_ = new Snapshot[nsnaps];
  for (size_t i = 0; i < nsnaps; ++i)
    F->snaps_[i] = buf->snaps_.at(i);
  F->snapmap_.data_ = buf->snapmap_.data_;
  F->snapmap_.index_ = buf->snapmap_.data_.size();

  AbstractHeap::compactCopyInto(&F->heap_, &buf->heap_);

  F->mcode_ = as->mcp;
#ifdef LC_TRACE_STATS
  F->stats_ = stats_;  // Transfers ownership.
  stats_ = NULL;
#endif

  return F;
}

Word *Jit::pushFrame(Word *base, BcIns *returnPc,
                     TRef noderef, uint32_t framesize) {
  int topslot = buf_.slots_.top();

  TRef ret_ref = buf_.literal(IRT_PC, (Word)returnPc);

  callStack_.pushFrame(ret_ref);

  buf_.setSlot(topslot + 0, buf_.baseLiteral(base));
  buf_.setSlot(topslot + 1, ret_ref);
  buf_.setSlot(topslot + 2, noderef);
  Word *newbase = base + topslot + 3;
  if (!buf_.slots_.frame(newbase, newbase + framesize)) {
    ++record_abort_reasons[AR_ABSTRACT_STACK_OVERFLOW];
    //    cerr << "Abstract stack overflow." << endl;
    return NULL;
  }
  return newbase;
}

int32_t
Jit::checkFreeHeapAvail(Fragment *F, SnapNo snapno)
{
  Snapshot &sn = F->snap(snapno);
  IRRef ref = sn.ref() - 1;
  int32_t used = 0;
  while (ref >= REF_FIRST) {
    IR *ins = F->ir(ref);
    if (ins->opcode() == IR::kNEW) {
      AbstractHeapEntry &entry = F->heap_.entry(ins->op2());
      used += 1 + entry.size();
    } else if (ins->opcode() == IR::kHEAPCHK) {
      used -= (int32_t)F->ir(ref)->op1();
      break;
    }
    --ref;
  }
  return used;
}

#undef DBG

#if (DEBUG_COMPONENTS & DEBUG_TRACE_ENTEREXIT) != 0
#define DBG(stmt) do { stmt; } while(0)
#else
#define DBG(stmt) do {} while(0)
#endif

inline uint64_t Fragment::literalValue(IRRef ref, Word *base) {
  IR *ins = ir(ref);
  if (ins->opcode() == IR::kKINT) {
    if (kOpIsSigned & (1 << (int)ins->type()))
      return (int64_t)(int32_t)ins->i32();
    else
      return (uint64_t)(uint32_t)ins->i32();
  } else if (ins->opcode() == IR::kKWORD) {
    return (uint64_t)ins->u32() | ((uint64_t)ir(ref - 1)->u32() << 32);
  } else if (ins->opcode() == IR::kKBASEO) {
    return (uint64_t)(base + ins->i32());
  }
  return 0;
}

static void printRegisters(ostream &out, Word *gpr) {
  for (RegSet work = kGPR; !work.isEmpty(); ) {
    Reg r = work.pickBot();
    work.clear(r);
    out << "    " << setw(3) << left << IR::regName(r, IRT_I64)
        << " = 0x"
        << setw(16) << setfill('0') << right << hex << gpr[r]
        << " / "
        << setw(20) << setfill(' ') << right << dec << (WordInt)gpr[r]
        << endl;
  }
}

static void printExitState(ostream &out, ExitState *ex) {
  Word *base = (Word *)ex->gpr[RID_BASE];
  Word *hp = (Word *)ex->gpr[RID_HP];
  out << "  base = " << base << ", hp = " << hp
      << ", hplim = " << ex->hplim
      << ", spill=" << ex->spill
      << " (delta=" << hex << (char *)ex->spill - (char *)base
      << endl;
  printRegisters(out, &ex->gpr[0]);
}

Word *traceDebugLastHp = NULL;

void Fragment::restoreSnapshot(ExitNo exitno, ExitState *ex) {
  Word *spill = ex->spill;
  LC_ASSERT(0 <= exitno && exitno < nsnaps_);
  DBG(cerr << "Restoring from snapshot " << (int)exitno
      << " of Trace " << traceId() << endl);
#ifdef LC_TRACE_STATS
  if (stats_ != NULL)
    bumpExitCount(exitno);
#endif
  Snapshot &sn = snap(exitno);
  IR *snapins = ir(sn.ref());
  Word *base = (Word *)ex->gpr[RID_BASE];
  traceDebugLastHp = NULL;
  if (snapins->opcode() != IR::kSAVE) {
    DBG(sn.debugPrint(cerr, &snapmap_, exitno));
    DBG(printExitState(cerr, ex));
    for (Snapshot::MapRef i = sn.begin(); i < sn.end(); ++i) {
      int slot = snapmap_.slotId(i);
      int ref = snapmap_.slotRef(i);
      IR *ins = ir(ref);
      DBG(cerr << "    Restoring "; IR::printIRRef(cerr, ref));
      DBG(cerr << ":  base[" << slot << "] = ");
      if (irref_islit(ref)) {
        uint64_t k = literalValue(ref, base);
        DBG(cerr << "literal (" << hex << k << ")" << endl);
        base[slot] = k;
      } else if (ins->spill() != 0) {
        DBG(cerr << "spill[" << (int)ins->spill() << "] ("
            << hex << spill[ins->spill()] << "/"
            << dec << spill[ins->spill()] << ")"
            << endl);
        base[slot] = spill[ins->spill()];
      } else {
        LC_ASSERT(isReg(ins->reg()));
        DBG(cerr << IR::regName(ins->reg(), ins->type()) << " ("
            << hex << ex->gpr[ins->reg()] << ")" << endl);
        base[slot] = ex->gpr[ins->reg()];
      }
    }
  }
  if (sn.relbase() != 0 && snapins->opcode() != IR::kSAVE) {
    DBG(cerr << "base + " << dec << (int)sn.relbase() << " => ");
    base += (int)sn.relbase();
    DBG(cerr << base << endl);
    // cerr << "NYI: non-zero relbase" << endl;
    // cerr << "   relbase = " << dec << sn.relbase() << endl;
    // exit(3);
  }
  ex->T->base_ = base;
  ex->T->top_ = base + sn.framesize();
  ex->T->pc_ = sn.pc();

  Capability *cap = ex->T->owner();
  LC_ASSERT(cap != NULL);
  cap->traceExitHp_ = (Word *)ex->gpr[RID_HP];
  cap->traceExitHpLim_ = ex->hplim;

  if (snapins->opcode() != IR::kHEAPCHK && sn.bumpExitCounter()) {
    if (snapins->opcode() == IR::kSAVE && snapins->op1() == IR_SAVE_FALLTHROUGH) {
      // If the parent trace falls back directly to the interpreter
      // then this new traces should be treated like a root trace.
      // The only difference is that the fallthrough branch should be
      // updated to jump directly to the entry of the new trace.
      BcIns *pc = sn.pc();
      if (pc->opcode() == BcIns::kJFUNC) {
        // A trace has been formed at the fall-through point before
        // the side exit got hot.
        Fragment *target = cap->jit()->lookupFragment(pc);
        LC_ASSERT(target && target->traceId() == pc->d());
        cap->jit()->patchFallthrough(this, exitno, target);
      } else {
        bool isReturn = !(pc->opcode() == BcIns::kFUNC || pc->opcode() == BcIns::kIFUNC);
        cap->jit()->beginRecording(cap, pc, base, isReturn);
        cap->jit()->setFallthroughParent(this, exitno);
        cap->setState(Capability::STATE_RECORD);
      }
    } else {
      DBG(cerr << COL_RED "HOTSIDE" COL_RESET "\n");
      cap->jit()->beginSideTrace(cap, base, this, exitno);
      cap->setState(Capability::STATE_RECORD);
    }
  }

  if (snapins->opcode() == IR::kHEAPCHK) {
    // cerr << "Heap check failure" << endl;
    // We exited due to a heap overflow.

    // TODO: If we only reached the end of a block, then we only
    // need to adjust r12 and HpLim.  This we could simply re-enter
    // the trace.  That's probably better handled via specialised
    // code in the codegen.  It's also a bit involved since we may
    // have to take the full exit if a GC is indeed required.

    // 1. Found out by how much we incremented.
    cap->traceExitHp_ -= (int)snapins->op1();

    // 2. We could directly grab a new block, but we have to be
    // careful about what happens if we trigger a GC.  So for now, we
    // let the interpreter handle all of this.
  }
}

#undef DBG

#define SLOT_SIZE (LC_ARCH_BITS/8)

static void asmSLOAD(Assembler *as, IR *ir) {
  int32_t ofs = SLOT_SIZE * (int16_t)ir->op1();
  Reg base = RID_BASE;
  RegSet allow = kGPR;
  Reg dst = as->destReg(ir, allow);
  as->load_u64(dst, base, ofs);
}

void Jit::genCode(IRBuffer *buf, IR *ir) {
  switch (ir->opcode()) {
  case IR::kSLOAD:
    asmSLOAD(&asm_, ir);
    break;
  default:
    exit(11);
    //    load_u64(
  }
}

extern "C" void LC_USED
debugTrace(ExitState *ex) {
  cerr << "dbgtrace: end of trace " << ex->F_id << endl;
  printExitState(cerr, ex);
  if (traceDebugLastHp != NULL) {
    cerr << "Allocated: " << endl;
    Word *hp = traceDebugLastHp;
    Word *newhp = (Word *)ex->gpr[RID_HP];
    int n = 0;
    while (hp < newhp) {
      if ((n % 4) == 0)
        cerr << hp << ":";
      cerr << " " << setw(16) << setfill('0') << right << hex << *hp;
      if ((++n % 4) == 0) cerr << endl;
      ++hp;
    }
    cerr << endl;
  }
  traceDebugLastHp = (Word *)ex->gpr[RID_HP];
}

static const char *nyiDescription[NYI__MAX] = {
#define NYIDESCR(name, descr) descr,
  NYIDEF(NYIDESCR)
#undef NYIDESCR
};

static uint64_t nyiCount[NYI__MAX] = {
#define NYICOUNT(name, descr) 0,
  NYIDEF(NYICOUNT)
#undef NYICOUNT
};

void
logNYI(uint32_t nyi_id)
{
  LC_ASSERT(nyi_id < NYI__MAX);
  ++nyiCount[nyi_id];
#ifndef NDEBUG
  fprintf(stderr, "%s\n", nyiDescription[nyi_id]);
#endif
}

void
printLoggedNYIs(FILE *out)
{
  for (uint32_t i = 0; i < NYI__MAX; ++i) {
    if (nyiCount[i] > 0)
      fprintf(out, "%"PRIu64" x NYI: %s\n", nyiCount[i], nyiDescription[i]);
  }
}

#ifdef LC_TRACE_STATS
uint64_t
Fragment::traceExits() const
{
  uint64_t exits = 0;
  for (uint32_t e = 0; e < nsnaps_; ++e) {
    exits += traceExitsAt(e);
  }
  return exits;
}
#endif

_END_LAMBDACHINE_NAMESPACE
