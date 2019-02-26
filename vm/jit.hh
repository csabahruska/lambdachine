#ifndef _JIT_H_
#define _JIT_H_

#include "common.hh"
#include "vm.hh"
#include "bytecode.hh"
#include "ir.hh"
#include "assembler.hh"
#include "objects.hh"

#include <vector>
#include <iostream>
#include HASH_MAP_H

_START_LAMBDACHINE_NAMESPACE

class HotCounters {
public:
  typedef uint16_t HotCount;
  static const Word kNumCounters = 1024; // Must be power of two.

  HotCounters(HotCount threshold);
  ~HotCounters() {}

  inline HotCount get(void *pc) const {
    return counters_[hotCountHash(pc)];
  }

  inline void set(void *pc, HotCount value) {
    counters_[hotCountHash(pc)] = value;
  }

  inline void reset(void *pc) {
    counters_[hotCountHash(pc)] = threshold_;
  }

  /// Decrement the hot counter.
  ///
  /// @return true if the counter reached the hotness threshold.
  inline bool tick(void *pc) {
    HotCount c = --counters_[hotCountHash(pc)];
    if (LC_UNLIKELY(c == 0)) {
      set(pc, threshold_);
      return true;
    } else {
      return false;
    }
  }

private:
  static inline Word hotCountHash(void *pc) {
    Word val = (Word)pc;
    return ((val >> 12) ^ (val >> 4)) & (kNumCounters - 1);
  }

  HotCount counters_[kNumCounters];
  HotCount threshold_;
};


// Very simple random number generator.
class Prng {
public:
  Prng() : state_(0x72ba83e) { }  // TODO: Make more random.
  Prng(uint32_t init) : state_(init) { }

  inline uint32_t bits(int nbits) {
    state_ = state_ * 1103515245 + 12345;
    return state_ >> (32 - nbits);
  }

private:
  uint32_t state_;
};


// Manages allocation and memory protection of the machine code area.
class MachineCode {
public:
  MachineCode(Prng *);
  ~MachineCode();

  static const size_t kAreaSize = (size_t)1 << 22; // 512KB

  /// Reserve the whole machine code area.  No code from the machine
  /// code area may be running at the same time.  (It will trigger a
  /// page fault.)
  ///
  /// @param limit Lower limit of the machine code area.
  /// @return Upper limit of the machine code area.
  MCode *reserve(MCode **limit);

  // Finish generation of machine code.
  //
  // @param top E
  void commit(MCode *top);
  void commitStub(MCode *bot);
  void abort();

  /// Reserves the machine code area containing the given pointer.
  /// Used to modify existing machine code (e.g., for trace linking).
  ///
  /// Returns a pointer to the beginning of the reserved area.
  MCode *patchBegin(MCode *ptr);

  /// Finish patching machine code.  The pointer argument should be
  /// the result of the matching patchBegin call.
  void patchFinish(MCode *area);

  // Synchronise data and instruction cache.
  void syncCache(void *start, void *end);

  /// Compiled fragment code is in the range. [start()-end()]
  inline MCode *start() const { return top_; }
  inline MCode *end() const { return (MCode*)((char*)area_ + size_); }

  /// Stub code is in the range [stubStart()-stubEnd()].
  ///
  /// INVARIANT: stubEnd() < start().
  inline MCode *stubStart() const { return area_; }
  inline MCode *stubEnd() const { return bottom_; }

  void dumpAsm(std::ostream &out);
  void dumpAsm(std::ostream &out, MCode *from, MCode *to);

private:
  void *alloc(size_t size);
  void free(void *p, size_t size);
  void *allocAt(uintptr_t hint, size_t size, int prot);
  void setProtection(void *p, size_t size, int prot);
  void allocArea();
  void protect(int prot);

  Prng *prng_;
  int protection_;
  MCode *area_;
  MCode *top_;
  MCode *bottom_;
  size_t size_;
  size_t sizeTotal_;
};


// Forward declarations.
class Capability;
class Fragment;

#define FRAGMENT_MAP \
  HASH_NAMESPACE::HASH_MAP_CLASS<Word,TraceId>

#define TRACE_ID_NONE  (~0)

typedef enum {
  TT_ROOT,
  TT_FALLTHROUGH,
  TT_SIDE,
} TraceType;

class Jit {
public:
  Jit();
  ~Jit();

  void beginRecording(Capability *, BcIns *startPc, Word *base,
                      bool isReturn);
  void beginSideTrace(Capability *, Word *base, Fragment *parent, SnapNo);

  // Returns true if recording finished
  bool recordIns(BcIns *, Word *base, const Code *);
  // bool recordGenericApply(uint32_t call_info, Word *base,
  //                         TRef fnode_ref, Closure *fnode,
  //                         const Code *code);
  bool recordGenericApply2(uint32_t call_info, Word *base,
                           TRef fnode_ref, Closure *fnode,
                           TRef *args, BcIns *returnPc);

  inline bool isRecording() const { return cap_ != NULL; }

  inline void requestAbort() { shouldAbort_ = true; }

  // Returns the fragment starting at the given PC. NULL, otherwise.
  inline Fragment *traceAt(BcIns *pc);

  static inline Fragment *traceById(TraceId traceId) {
    LC_ASSERT(traceId < fragments_.size());
    return fragments_[traceId];
  }

  typedef enum {
    kOptDebugTrace,
    kOptFastHeapCheckFail
  } JitOption;

  inline void setOption(JitOption option, bool value) {
    options_.set((int)option, value);
  }

  inline bool getOption(JitOption option) const {
    return options_.get((int)option);
  }

  inline MachineCode *mcode() { return &mcode_; }
  inline IRBuffer *buffer() { return &buf_; }
  inline Assembler *assembler() { return &asm_; }

  Fragment *saveFragment();
  Fragment *lookupFragment(BcIns *pc) {
    Word idx = reinterpret_cast<Word>(pc) >> 2;
    return fragments_[fragmentMap_[idx]];
  }

  inline void setDebugTrace(bool val) { options_.set(kOptDebugTrace, val); }
  static inline void registerFragment(BcIns *startPc, Fragment *F, bool isSideTrace);
  static void resetFragments();
  static uint32_t numFragments();

  void setFallthroughParent(Fragment *parent, SnapNo snapno);
  void patchFallthrough(Fragment *parent, ExitNo exitno, Fragment *target);

private:
  void initRecording(Capability *cap, Word *base, BcIns *startPc);
  Word *pushFrame(Word *base, BcIns *returnPc, TRef noderef,
                  uint32_t framesize);
  void finishRecording();
  void resetRecorderState();
  void replaySnapshot(Fragment *parent, SnapNo snapno, Word *base);
  int32_t checkFreeHeapAvail(Fragment *F, SnapNo snapno);

  static const int kLastInsWasBranch = 0;
  static const int kIsReturnTrace = 1;

  Capability *cap_;
  BcIns *startPc_;
  Word *startBase_;
  Fragment *parent_;
  ExitNo parentExitNo_;
  Flags32 flags_;   // reset each time
  Flags32 options_; // configuration options
  TraceType traceType_;
  std::vector<BcIns*> targets_;
  Prng prng_;
  MachineCode mcode_;
  IRBuffer buf_;
  Assembler asm_;
  CallStack callStack_;
  BranchTargetBuffer btb_;
  MCode *exitStubGroup_[16];
  bool shouldAbort_;
#ifdef LC_TRACE_STATS
  uint64_t *stats_;
#endif

  static FRAGMENT_MAP fragmentMap_;
  static std::vector<Fragment*> fragments_;

  void genCode(IRBuffer *buf);
  void genCode(IRBuffer *buf, IR *ir);

  friend class Assembler;
};

inline Fragment *Jit::traceAt(BcIns *pc) {
  Word idx = reinterpret_cast<Word>(pc) >> 2;
  FRAGMENT_MAP::const_iterator it = fragmentMap_.find(idx);
  if (it != fragmentMap_.end())
    return fragments_[it->second];
  else
    return NULL;
}

typedef struct _ExitState ExitState; // architecture-specific.

class Fragment {
public:
  inline uint32_t traceId() const { return traceId_; }
  inline bool isCompiled() const { return flags_.get(kIsCompiled); }
  inline bool isSimulated() const { return !flags_.get(kIsCompiled); }

  // Not sure if we really need to store targets in the final trace.
  // They're really only useful for loop truncation.
  inline uint32_t targetCount() const { return numTargets_; }
  inline BcIns *target(uint32_t n) const { return targets_[n]; }

  /// Maximum amount of stack space needed by this fragment. This is
  /// the sum of all spill slots and the highest snapshot write.
  ///
  /// TODO: What if a side trace needs to increase this value?
  inline uint16_t frameSize() const { return frameSize_; }

  inline BcIns *startPc() const { return startPc_; }

  inline MCode *entry() { return mcode_; }
  uint64_t literalValue(IRRef, Word* base);
  void restoreSnapshot(ExitNo, ExitState *);

  ~Fragment();

  inline Snapshot &snap(SnapNo n) {
    LC_ASSERT(n < nsnaps_);
    return snaps_[n];
  }

  inline uint32_t numExits() const { return nsnaps_; }

#ifdef LC_TRACE_STATS
  inline uint64_t traceCompletions() const { return stats_[0]; }
  inline uint64_t traceExitsAt(ExitNo n) const {
    LC_ASSERT(n < nsnaps_);
    return stats_[1 + n];
  }
  uint64_t traceExits() const;
  inline Fragment *parent() const { return parent_; }

private:
  void bumpExitCount(ExitNo n) { ++stats_[1 + n]; }
  uint64_t *exitCounterAddress(ExitNo n) const { return &stats_[1 + n]; }
#endif

private:
  Fragment();

  inline IR *ir(IRRef ref) { return &buffer_[ref]; }

  static const int kIsCompiled = 1;

  Flags32 flags_;
  uint32_t traceId_;
  BcIns *startPc_;
  Fragment *parent_;
  // ExitNo parentExit_;

  BcIns **targets_;
  uint32_t numTargets_;

  IRRef firstconstant_;  // Lowest IR constant. Biased with REF_BIAS
  IRRef nextins_;        // Next IR instruction. Biased with REF_BIAS
  IR *buffer_;           // Biased buffer

  uint16_t frameSize_;
  uint16_t nsnaps_;
  Snapshot *snaps_;
  SnapshotData snapmap_;
  AbstractHeap heap_;

  MCode *mcode_;
  //  size_t sizemcode_;

#ifdef LC_TRACE_STATS
  uint64_t *stats_;
#endif

  friend class Jit;
  friend class Assembler;
};

inline void Jit::registerFragment(BcIns *startPc, Fragment *F, bool isSideTrace) {
  LC_ASSERT(F->traceId() == fragments_.size());
  fragments_.push_back(F);
  Word idx = reinterpret_cast<Word>(startPc) >> 2;
  if (!isSideTrace) {
    fragmentMap_[idx] = F->traceId();
  }
#if (DEBUG_COMPONENTS & DEBUG_TRACE_CREATION)
  std::cerr << "ADD TRACE " << F->traceId() << " pc=" << startPc;
  if (F->parent_ != NULL) {
    std::cerr << " parent=" << F->parent_->traceId()
      // << " @ exit " << F->parentExitNo()
              << std::endl;
  } else {
    std::cerr << " (root trace)\n";
  }
#endif
}

/* This definition must match the asmEnter/asmExit functions */
struct _ExitState {
  double   fpr[RID_NUM_FPR];    /* Floating-point registers. */
  Word     gpr[RID_NUM_GPR];    /* General-purpose registers. */
  Word     *hplim;              /* Heap Limit */
  Word     *stacklim;           /* Stack Limit */
  Word     unused1;
  Thread   *T;                  /* Currently executing thread */
  TraceId  F_id;                /* Fragment under execution */
  uint32_t unused2;             // Padding
  Word     spill[256];
};

typedef enum {
  AR_ABSTRACT_STACK_OVERFLOW,
  AR_KNOWN_TO_FAIL_GUARD,
  AR_TRACE_TOO_LONG,
  AR_INTERPRETER_REQUEST,
  AR_NYI,
  AR__MAX
} AbortReason;

extern uint64_t record_aborts;
extern uint64_t record_abort_reasons[AR__MAX];

#define HPLIM_SP_OFFS  0
#define SPLIM_SP_OFFS  8
#define SPILL_SP_OFFS  (offsetof(ExitState, spill) - offsetof(ExitState, hplim))
#define F_ID_OFFS      (offsetof(ExitState, F_id) - offsetof(ExitState, hplim))

extern "C" void asmEnter(TraceId F_id, Thread *T,
                         Word *hp, Word *hplim, Word *stacklim, MCode *code);

extern "C" void asmExit(int);

extern "C" void asmHeapOverflow(void);
extern "C" void asmStackOverflow(void);
extern "C" void asmTrace(void);
extern "C" void debugTrace(ExitState *);

_END_LAMBDACHINE_NAMESPACE

#endif /* _JIT_H_ */
