#ifndef _THREAD_H_
#define _THREAD_H_

#include "common.hh"
#include "vm.hh"
#include "bytecode.hh"

_START_LAMBDACHINE_NAMESPACE

class Thread {

public:
  static const Word kMinStackWords = 64;

  static Thread *createThread(Capability *, Word stackSizeInWords);
  static Thread *createTestingThread(BcIns *pc, u4 framesize);
  ~Thread();

  inline BcIns *pc() const { return pc_; }
  inline Word *stackStart() const { return stack_; }
  inline Word *stackLimit() const { return stack_ + stackSize_; }
  inline Word *top() const { return top_; }
  inline Word *base() const { return base_; }

  inline bool isValidSlot(int32_t n) const {
    return n >= -1 && (base_ + n < top_);
  }
  bool isValid() const;

  inline Word slot(int32_t n) const { return base_[n]; }
  inline void setSlot(int32_t n, Word value) {
    base_[n] = value;
  }

  inline Word lastResult() const { return lastResult_; }
  inline void setLastResult(Word value) { lastResult_ = value; }
  inline void setPC(BcIns *pc) { pc_ = pc; }

private:
  Thread(Word stackSizeInWords);
  void initialize();

  inline void sync(BcIns *pc, Word *base) {
    pc_ = pc; base_ = base;
  }

  static BcIns stopCode_[];

  friend class Capability;

  Word header_;
  BcIns *pc_;
  Word stackSize_;
  Word *base_;
  Word *top_;
  Word lastResult_;
  Word *stack_;
};

_END_LAMBDACHINE_NAMESPACE

#endif /* _THREAD_H_ */
