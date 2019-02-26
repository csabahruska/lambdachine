#ifndef _LAMBDACHINE_JIT_H
#define _LAMBDACHINE_JIT_H

#include "Common.h"
#include "VM.h"
#include "IR.h"
#include "Bytecode.h"
#include "InfoTables.h"
#include "Opts.h"

#include <stdio.h>
#include <stdlib.h>

#define INITIAL_BASE 100
#define MAX_SLOTS    200
#define MAX_TRACE_LENGTH 2000

// -- Hot counters ---------------------------------------------------
// Hot counters are stored in a Capability, not globally, to avoid
// cache contention issues.
typedef u2 HotCount;

// Number of hot counter hash table entries (power of two)
#define HOTCOUNT_SIZE           64
#define HOTCOUNT_PCMASK         ((HOTCOUNT_SIZE-1)*sizeof(HotCount))
// Initial value of hot counter.  Need data to figure out default.
// Will be customisable one day.
#define HOTCOUNT_DEFAULT        7

// -- JIT stuff ------------------------------------------------------

// The header structure describing the data in the snapshot map.
//
// In addition to the snapshot references, the snapshot map also
// stores:
//  - the program counter (as a 32bit offset from the startpc)
//  - the base slot relative to INITIAL_BASE
//  - all slots are relative to INITIAL_BASE, too
typedef struct _SnapShot {
  u2     mapofs; // Start offset into snapshot map.
  IRRef1 ref;    // First IR reference for this snapshot
  u1     nslots; // Number of valid slots;
  u1     nent;   // Number of compressed entries.
  i1     minslot; // Slots captured are in [minslot, minslot + nslots - 1]
  u1     count;  // Number of taken exits for this snapshot.
} SnapShot;

INLINE_HEADER bool snapShotRemoved(SnapShot *snap) {
  return snap->nslots == 0;
}

INLINE_HEADER void snapShotMarkRemoved(SnapShot *snap) {
  snap->nslots = 0;
}

typedef u4 SnapEntry;

// Construct SnapEntry from a slot and a tagged reference.
#define SNAP_TR(slot, tr) \
  (((SnapEntry)(slot) << 16) | ((SnapEntry)tref_ref(tr)))

#define snap_ref(sn)            ((sn) & 0xffff)
#define snap_slot(sn)           (cast(i2, ((sn) >> 16)))

/*
 * Heap Info
 * ---------
 *
 * The heap info is an abstraction for the contents of the heap.  Heap
 * info entries are initialised when an object is allocated and will
 * not be modified after that.  To support UPDATE operations, a special field
 * is reserved to point to the new new value if needed.
 *
 *
 */

typedef IRRef1 HeapEntry;

#define heap_ref(hr)            (hr)

typedef struct _HeapInfo {
  u2 mapofs;
  IRRef1 ref; // First reference to heap object
  int hp_offs; /* Start address of object as offset (in words) from
                 heap pointer at beginning of trace/loop */
  u1 nfields; // Total number of fields
  u1 nent;    // Number of `HeapEntry`s used
  u1 compact; // non-zero if fields are in order.
  u1 loop;
  IRRef1 ind; // Points to the new object after an UPDATE.
  u2 dfs;
  u2 scc;
} HeapInfo;

/* Snapshot and exit numbers. */
typedef u4 SnapNo; /* Snapshot numbers */
typedef u4 ExitNo; /* Trace exit numbers */

/* Machine code types. */
typedef u1 MCode;  /* Type for storing Machine code */
typedef u4 MSize;  /* Machine code size */

typedef enum {
  FUNCTION_TRACE,
  RETURN_TRACE,
  SIDE_TRACE
} TraceType;

typedef enum {
  BRANCH_CALL,
  BRANCH_RETURN
} BranchType;

/* Fragments */
typedef u2 FragmentId;

typedef struct _Fragment {
  IRIns *ir;
  IRRef nins;  // Next IR instruction
  IRRef nk;    // Lowest IR literal
  IRRef nloop; // Reference to LOOP instruction (if any)
  u2 nphis;    // Number of PHI nodes (only needed by IR interpreter)
  u2 fragmentid;

  Word *kwords; // Constant words
  u4 nkwords;

  u2 nsnap;    // Number of snapshots
  u2 nsnapmap; // Number of snapshot map elements
  SnapShot *snap;    // Snapshot array.
  SnapEntry *snapmap; // Snapshot map array.

  const BCIns *startpc; // needed for snapshot decoding
  BCIns orig;  // Original instruction at trace start

  u2 nheap;
  u2 nheapmap;
  HeapInfo *heap;
  HeapEntry *heapmap;
  u4 framesize; // Number of frame slots used by this fragment (before RA)
  u4 spills;    // Number of spill slots used (added by RA)

  MCode *mcode;  // Machine code for the trace
  MSize szmcode; // Size of machine code
  u2 traceType;
} Fragment;

/* Fold state is used to fold instructions on-the-fly. */
typedef struct _FoldState {
  IRIns ins;
  IRIns left;
  IRIns right;
} FoldState;

/* Optimization parameters and their defaults. Length is a char in octal! */
#define JIT_PARAMDEF(_) \
  _(\011, enableasm,     0)     /* Generate machine code for traces. */ \
  _(\012, enableside,    0)     /* Enable side traces. */ \
  /* Size of each machine code area (in KBytes). */ \
  _(\011, sizemcode,    64) \
  /* Max. total size of all machine code areas (in KBytes). */ \
  _(\010, maxmcode,     512) \
  /* End of list. */

enum {
#define JIT_PARAMENUM(len, name, value) JIT_P_##name,
JIT_PARAMDEF(JIT_PARAMENUM)
#undef JIT_PARAMENUM
  JIT_P__MAX
};
#define JIT_PARAMSTR(len, name, value)  #len #name
#define JIT_P_STRING    JIT_PARAMDEF(JIT_PARAMSTR)

// -- Optimisations --------------------------------------------------

#define JIT_OPT_FIRST           1

// Perform dead code elimination
#define JIT_OPT_DCE             (1UL<<0)

// Loop unrolling (if possible)
#define JIT_OPT_UNROLL          (1UL<<1)

// Try to eliminate allocations by delaying them (only available when
// unrolling loops).
#define JIT_OPT_SINK_ALLOC      (1UL<<2)

#define JIT_OPT_CALL_BY_NAME    (1UL<<3)

#define JIT_OPT_CSE             (1UL<<4)

#define JIT_OPT_DEFAULT \
   (JIT_OPT_DCE|JIT_OPT_UNROLL|JIT_OPT_SINK_ALLOC|JIT_OPT_CSE)

#define JIT_OPTSTRING \
   "\3dce\6unroll\4sink\3cbn\3cse"


typedef enum {
  JIT_MODE_NORMAL,
  JIT_MODE_RECORDING,
  JIT_MODE_VERIFY,
} JitMode;

/* JIT compiler state. */
typedef struct _JitState {
  Fragment cur;

  // Current VM state
  Thread *T;
  const BCIns *pc;
  FuncInfoTable *func;

  // Virtual/Recorder State
  TRef *base;      // current base pointer as pointer into slots
  TRef slot[MAX_SLOTS];   // virtual register contents
  u1 baseslot;  // current base pointer as offset into slot
  u1 maxslot;   // size of the current frame
                   // INVARIANT: baseslot + maxslot < MAX_SLOTS
  u1 minslot;   // index into slot, INV: minslot >= 0 && minslot < baseslot
  u4 framesize;    /* Max. stack used by this trace. */
  TRef last_result;
  u4 flags;
  u4 mode;
  i4 framedepth;
  u4 unroll;

  // IR Buffer.
  //
  // INVARIANTS:
  //   size = irmax - irmin;
  //   size != 0 ==> alloc_ptr =
  IRIns *irbuf;
  IRRef irmin;
  IRRef irmax;

  u4 sizekwords;
  Word *kwordsbuf;

  u1 needsnap;
  u1 mergesnap;
  u1 unused1;
  u1 unused2;

  // Snapshot buffer.
  //
  // The difference between this and `cur.snap`/`cur.snapmap` is that
  // these always point at the beginning of the buffer, while the
  // stuff inside `cur` might point somewhere in the middle.
  Word sizesnap;
  SnapShot *snapbuf;
  SnapEntry *snapmapbuf;
  Word sizesnapmap;

  Word sizeheap;
  Word sizeheapmap;
  HeapInfo *heapbuf;
  HeapEntry *heapmapbuf;

  BCIns *startpc; // Address where recording was started.
  const Word *startbase;

  FoldState fold;
  IRRef1 chain[IR__MAX];

  FILE *loghandle;

  // Code cache
  Fragment **fragment;
  u4 nfragments;       // number of entries used
  u4 sizefragment;     // total size of table, power of 2

  MCode *exitstubgroup[LC_MAX_EXITSTUBGR];  /* Exit stub group addresses. */

  int mcprot;           /* Protection of current mcode area. */
  MCode *mcarea;        /* Base of current mcode area. */
  MCode *mctop;         /* Top of current mcode area. */
  MCode *mcbot;         /* Bottom of current mcode area. */
  size_t szmcarea;      /* Size of current mcode area. */
  size_t szallmcarea;   /* Total size of all allocated mcode areas. */

  uint32_t prngstate;   /* PRNG state. */

  Word optimizations;
  int32_t param[JIT_P__MAX];  /* JIT engine parameters. */
} JitState;

#define LOG_JIT(J, ...) \
  do { if ((J)->loghandle) { fprintf((J)->loghandle, __VA_ARGS__); } }  \
  while(0)

/* Trivial PRNG e.g. used for penalty randomization. */
static LC_AINLINE uint32_t LC_PRNG_BITS(JitState *J, int bits)
{
  /* Yes, this LCG is very weak, but that doesn't matter for our use case. */
  J->prngstate = J->prngstate * 1103515245 + 12345;
  return J->prngstate >> (32-bits);
}

typedef struct _FragmentEntry {
  u2 unused;
  u2 chain;
  const BCIns *pc;
  Fragment *code;
} FragmentEnty;

typedef enum {
  REC_ABORT = 0,  // Recording has been aborted
  REC_CONT  = 1,  // Continue recording
  REC_LOOP  = 2,  // Loop detected, continue at trace in higher bits
  REC_DONE  = 3,  // Recording finished but not with a loop.
  REC_MASK  = 0xff
} RecordResult;

typedef enum {
  UNROLL_DISABLED = 0,
  UNROLL_ONCE = 1
} UnrollLevel;

INLINE_HEADER FragmentId getFragmentId(RecordResult r) { return (u4)r >> 8; }

void initJitState(JitState *J, const Opts* opts);
LC_FASTCALL void startRecording(JitState *J, BCIns *, Thread *, Word *base,
                                TraceType type);
LC_FASTCALL void
startRecordingSideTrace(JitState *J, Thread *T, Word *base,
                        Fragment *parent, uint32_t exitno);

void recordSetup(JitState *J, Thread *T);
FragmentId finishRecording(JitState *J, UnrollLevel unroll);
LC_FASTCALL TRef emitIR(JitState *J);
LC_FASTCALL TRef emitLoadSlot(JitState *J, i4 slot);
LC_FASTCALL TRef emitKWord_(JitState *J, Word w, IRType t);

INLINE_HEADER IRType
litTypeToIRType(LitType lt)
{
  switch (lt) {
  case LIT_INT:     return IRT_I32;
  case LIT_STRING:  return IRT_PTR;
  case LIT_CHAR:    return IRT_I32;
  case LIT_WORD:    return IRT_U32;
  case LIT_FLOAT:   return IRT_F32;
  case LIT_INFO:    return IRT_INFO;
  case LIT_CLOSURE: return IRT_CLOS;
  case LIT_PC:      return IRT_PC;
  default: LC_ASSERT(0); return 0;
  }
}

INLINE_HEADER
TRef
emitKWord(JitState *J, Word w, LitType lt) {
  return emitKWord_(J, w, litTypeToIRType(lt));
}

RecordResult recordIns(JitState *J);

LC_FASTCALL TRef optFold(JitState *J);
LC_FASTCALL TRef optCSE(JitState *);
LC_FASTCALL void optUnrollLoop(JitState *J);
#define PRE_ALLOC_SINK   false
#define POST_ALLOC_SINK  true
LC_FASTCALL void optDeadCodeElim(JitState *J, bool post_sink);
LC_FASTCALL void optDeadAssignElim(JitState *J);
LC_FASTCALL TRef optForward(JitState *J);
LC_FASTCALL void compactPhis(JitState *J);

void growIRBufferTop(JitState *J);

int irEngine(Capability *cap, Fragment *F);

INLINE_HEADER TRef getSlot(JitState *J, int slot)
{
  return J->base[slot] ? J->base[slot] : emitLoadSlot(J, slot);
}

INLINE_HEADER void setSlot(JitState *J, int slot, TRef ref)
{
  //printf("Setting slot: %d (%ld) to %d\n",
  //       slot, &J->base[slot] - J->slot, (IRRef1)ref - REF_BIAS);
  J->base[slot] = ref != 0 ? (ref | TREF_WRITTEN) : ref;
  if (slot >= J->maxslot) J->maxslot = slot + 1;
}

INLINE_HEADER void clearSlot(JitState *J, int slot) {
  setSlot(J, slot, 0);
}


// Put instruction in the folding slot.
INLINE_HEADER void setFoldIR(JitState *J, u2 ot, IRRef1 a, IRRef1 b)
{
  J->fold.ins.ot = ot;  J->fold.ins.op1 = a;  J->fold.ins.op2 = b;
}

// Emit instruction without optimisation.
INLINE_HEADER TRef emit_raw(JitState *J, u2 ot, IRRef1 a, IRRef1 b)
{
  setFoldIR(J, ot, a, b);
  return emitIR(J);
}

// Emit instruction with optimisations.
INLINE_HEADER TRef emit(JitState *J, u2 ot, IRRef1 a, IRRef1 b)
{
  setFoldIR(J, ot, a, b);
  return optFold(J);
}

INLINE_HEADER void traceError(JitState *J, int n)
{
  exit(n);
}

LC_FASTCALL IRRef findPhi_aux(JitState *J, IRRef ref);

// Find the corresponding PHI node for the given target reference.
//
// Returns a non-zero result iff [ref] is shadowed by a PHI node.
// The result is a reference to the PHI node itself.
INLINE_HEADER IRRef
findPhi(JitState *J, IRRef ref)
{
  if (ref < REF_BIAS || ref >= J->cur.nloop || !irt_getphi(J->cur.ir[ref].t))
    return 0;

  return findPhi_aux(J, ref);
}

// Find the corresponding twin of a referenced involved in a PHI node.
//
// Returns a non-zero result iff [ref] is a PHI node.  The returned
// reference is the PHI twin (i.e., the second argument to the PHI
// node).
INLINE_HEADER IRRef
findPhiTwin(JitState *J, IRRef ref)
{
  IRRef ref2 = findPhi(J, ref);
  return ref != 0 ? J->cur.ir[ref2].op2 : 0;
}

typedef enum {
  STEP_START_RECORDING   = 0x0001,
  STEP_FINISH_RECORDING  = 0x0002,
  STEP_ENTER_TRACE       = 0x0004,
  STEP_EXIT_TRACE        = 0x0008
} JitStep;

extern u4 G_jitstep;

void printIRBuffer(JitState *J);
bool checkPerOpcodeLinks(JitState *J);

bool parseJitOpt(int32_t *param, uint32_t *flags, const char *str);
void setJitOpts(JitState *J, int32_t *param, uint32_t flags);

BCIns *
interpreterBranch(Capability *cap, JitState *J, BCIns *src_pc,
                  BCIns *dst_pc, Word *base, BranchType branchType);


#ifdef LC_SELF_CHECK_MODE

#define MAX_SHADOW_HEAP_SIZE (1 << 16)

void initShadowHeap(void);
Word lookupShadowHeap(Word *address);
void writeToShadowHeap(Word *address, Word value);
void resetShadowHeap(Word *hp, Word *hp_lim);
Word *allocOnShadowHeap(Word nwords);
bool verifyShadowHeap();
void printShadowHeap(FILE *stream);
void initShadowStack(u4 size, Word *stack, Word *base);
Word readStack(Word *base, int slot);
void writeStack(Word *base, int slot, Word value);
bool checkShadowSlot(Word *slot);
bool verifyShadowStack();
bool verifyShadowHeap();
void clearShadowStack(void);

#endif


#endif
