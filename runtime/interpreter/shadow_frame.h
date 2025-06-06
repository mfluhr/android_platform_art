/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_INTERPRETER_SHADOW_FRAME_H_
#define ART_RUNTIME_INTERPRETER_SHADOW_FRAME_H_

#include <cstdint>
#include <cstring>
#include <string>

#include "base/locks.h"
#include "base/macros.h"
#include "lock_count_data.h"
#include "read_barrier.h"
#include "stack_reference.h"
#include "verify_object.h"

namespace art HIDDEN {

namespace mirror {
class Object;
}  // namespace mirror

class ArtMethod;
class ShadowFrame;
template<class MirrorType> class ObjPtr;
class Thread;
union JValue;

// Forward declaration. Just calls the destructor.
struct ShadowFrameDeleter;
using ShadowFrameAllocaUniquePtr = std::unique_ptr<ShadowFrame, ShadowFrameDeleter>;

// ShadowFrame has 2 possible layouts:
//  - interpreter - separate VRegs and reference arrays. References are in the reference array.
//  - JNI - just VRegs, but where every VReg holds a reference.
class ShadowFrame {
 private:
  // Used to keep track of extra state the shadowframe has.
  enum class FrameFlags : uint32_t {
    // We have been requested to notify when this frame gets popped.
    kNotifyFramePop = 1 << 0,
    // We have been asked to pop this frame off the stack as soon as possible.
    kForcePopFrame = 1 << 1,
    // We have been asked to re-execute the last instruction.
    kForceRetryInst = 1 << 2,
    // Mark that we expect the next frame to retry the last instruction (used by instrumentation and
    // debuggers to keep track of required events)
    kSkipMethodExitEvents = 1 << 3,
    // Used to suppress exception events caused by other instrumentation events.
    kSkipNextExceptionEvent = 1 << 4,
    // Used to specify if DexPCMoveEvents have to be reported. These events will
    // only be reported if the method has a breakpoint set.
    kNotifyDexPcMoveEvents = 1 << 5,
    // Used to specify if ExceptionHandledEvent has to be reported. When enabled these events are
    // reported when we reach the catch block after an exception was thrown. These events have to
    // be reported after the DexPCMoveEvent if enabled.
    kNotifyExceptionHandledEvent = 1 << 6,
  };

 public:
  // Compute size of ShadowFrame in bytes assuming it has a reference array.
  static size_t ComputeSize(uint32_t num_vregs) {
    return sizeof(ShadowFrame) + (sizeof(uint32_t) * num_vregs) +
           (sizeof(StackReference<mirror::Object>) * num_vregs);
  }

  // Create ShadowFrame in heap for deoptimization.
  static ShadowFrame* CreateDeoptimizedFrame(uint32_t num_vregs,
                                             ArtMethod* method,
                                             uint32_t dex_pc) {
    uint8_t* memory = new uint8_t[ComputeSize(num_vregs)];
    return CreateShadowFrameImpl(num_vregs, method, dex_pc, memory);
  }

  // Delete a ShadowFrame allocated on the heap for deoptimization.
  static void DeleteDeoptimizedFrame(ShadowFrame* sf) {
    sf->~ShadowFrame();  // Explicitly destruct.
    uint8_t* memory = reinterpret_cast<uint8_t*>(sf);
    delete[] memory;
  }

  // Create a shadow frame in a fresh alloca. This needs to be in the context of the caller.
  // Inlining doesn't work, the compiler will still undo the alloca. So this needs to be a macro.
#define CREATE_SHADOW_FRAME(num_vregs, method, dex_pc) ({                                    \
    size_t frame_size = ShadowFrame::ComputeSize(num_vregs);                                 \
    void* alloca_mem = alloca(frame_size);                                                   \
    ShadowFrameAllocaUniquePtr(                                                              \
        ShadowFrame::CreateShadowFrameImpl((num_vregs), (method), (dex_pc), (alloca_mem)));  \
    })

  ~ShadowFrame() {}

  uint32_t NumberOfVRegs() const {
    return number_of_vregs_;
  }

  uint32_t GetDexPC() const { return dex_pc_; }

  void SetDexPC(uint32_t dex_pc) { dex_pc_ = dex_pc; }

  ShadowFrame* GetLink() const {
    return link_;
  }

  void SetLink(ShadowFrame* frame) {
    DCHECK_NE(this, frame);
    DCHECK_EQ(link_, nullptr);
    link_ = frame;
  }

  void ClearLink() {
    link_ = nullptr;
  }

  int32_t GetVReg(size_t i) const {
    DCHECK_LT(i, NumberOfVRegs());
    const uint32_t* vreg = &vregs_[i];
    return *reinterpret_cast<const int32_t*>(vreg);
  }

  // Shorts are extended to Ints in VRegs.  Interpreter intrinsics needs them as shorts.
  int16_t GetVRegShort(size_t i) const {
    return static_cast<int16_t>(GetVReg(i));
  }

  uint32_t* GetVRegAddr(size_t i) {
    return &vregs_[i];
  }

  uint32_t* GetShadowRefAddr(size_t i) {
    DCHECK_LT(i, NumberOfVRegs());
    return &vregs_[i + NumberOfVRegs()];
  }

  float GetVRegFloat(size_t i) const {
    DCHECK_LT(i, NumberOfVRegs());
    // NOTE: Strict-aliasing?
    const uint32_t* vreg = &vregs_[i];
    return *reinterpret_cast<const float*>(vreg);
  }

  int64_t GetVRegLong(size_t i) const {
    DCHECK_LT(i + 1, NumberOfVRegs());
    const uint32_t* vreg = &vregs_[i];
    using unaligned_int64 __attribute__((aligned(4))) = const int64_t;
    return *reinterpret_cast<unaligned_int64*>(vreg);
  }

  double GetVRegDouble(size_t i) const {
    DCHECK_LT(i + 1, NumberOfVRegs());
    const uint32_t* vreg = &vregs_[i];
    using unaligned_double __attribute__((aligned(4))) = const double;
    return *reinterpret_cast<unaligned_double*>(vreg);
  }

  // Look up the reference given its virtual register number.
  // If this returns non-null then this does not mean the vreg is currently a reference
  // on non-moving collectors. Check that the raw reg with GetVReg is equal to this if not certain.
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  mirror::Object* GetVRegReference(size_t i) const REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK_LT(i, NumberOfVRegs());
    mirror::Object* ref;
    ref = References()[i].AsMirrorPtr();
    ReadBarrier::MaybeAssertToSpaceInvariant(ref);
    if (kVerifyFlags & kVerifyReads) {
      VerifyObject(ref);
    }
    return ref;
  }

  // Get view of vregs as range of consecutive arguments starting at i.
  uint32_t* GetVRegArgs(size_t i) {
    return &vregs_[i];
  }

  void SetVReg(size_t i, int32_t val) {
    DCHECK_LT(i, NumberOfVRegs());
    uint32_t* vreg = &vregs_[i];
    *reinterpret_cast<int32_t*>(vreg) = val;
    // This is needed for moving collectors since these can update the vreg references if they
    // happen to agree with references in the reference array.
    References()[i].Clear();
  }

  void SetVRegFloat(size_t i, float val) {
    DCHECK_LT(i, NumberOfVRegs());
    uint32_t* vreg = &vregs_[i];
    *reinterpret_cast<float*>(vreg) = val;
    // This is needed for moving collectors since these can update the vreg references if they
    // happen to agree with references in the reference array.
    References()[i].Clear();
  }

  void SetVRegLong(size_t i, int64_t val) {
    DCHECK_LT(i + 1, NumberOfVRegs());
    uint32_t* vreg = &vregs_[i];
    using unaligned_int64 __attribute__((aligned(4))) = int64_t;
    *reinterpret_cast<unaligned_int64*>(vreg) = val;
    // This is needed for moving collectors since these can update the vreg references if they
    // happen to agree with references in the reference array.
    References()[i].Clear();
    References()[i + 1].Clear();
  }

  void SetVRegDouble(size_t i, double val) {
    DCHECK_LT(i + 1, NumberOfVRegs());
    uint32_t* vreg = &vregs_[i];
    using unaligned_double __attribute__((aligned(4))) = double;
    *reinterpret_cast<unaligned_double*>(vreg) = val;
    // This is needed for moving collectors since these can update the vreg references if they
    // happen to agree with references in the reference array.
    References()[i].Clear();
    References()[i + 1].Clear();
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetVRegReference(size_t i, ObjPtr<mirror::Object> val)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void SetMethod(ArtMethod* method) REQUIRES(Locks::mutator_lock_) {
    DCHECK(method != nullptr);
    DCHECK(method_ != nullptr);
    method_ = method;
  }

  ArtMethod* GetMethod() const REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(method_ != nullptr);
    return method_;
  }

  mirror::Object* GetThisObject() const REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::Object* GetThisObject(uint16_t num_ins) const REQUIRES_SHARED(Locks::mutator_lock_);

  bool Contains(StackReference<mirror::Object>* shadow_frame_entry_obj) const {
    return ((&References()[0] <= shadow_frame_entry_obj) &&
            (shadow_frame_entry_obj <= (&References()[NumberOfVRegs() - 1])));
  }

  LockCountData& GetLockCountData() {
    return lock_count_data_;
  }

  static constexpr size_t LockCountDataOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, lock_count_data_);
  }

  static constexpr size_t LinkOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, link_);
  }

  static constexpr size_t MethodOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, method_);
  }

  static constexpr size_t DexPCOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, dex_pc_);
  }

  static constexpr size_t NumberOfVRegsOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, number_of_vregs_);
  }

  static constexpr size_t VRegsOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, vregs_);
  }

  // Create ShadowFrame for interpreter using provided memory.
  static ShadowFrame* CreateShadowFrameImpl(uint32_t num_vregs,
                                            ArtMethod* method,
                                            uint32_t dex_pc,
                                            void* memory) {
    return new (memory) ShadowFrame(num_vregs, method, dex_pc);
  }

  bool NeedsNotifyPop() const {
    return GetFrameFlag(FrameFlags::kNotifyFramePop);
  }

  void SetNotifyPop(bool notify) {
    UpdateFrameFlag(notify, FrameFlags::kNotifyFramePop);
  }

  bool GetForcePopFrame() const {
    return GetFrameFlag(FrameFlags::kForcePopFrame);
  }

  void SetForcePopFrame(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kForcePopFrame);
  }

  bool GetForceRetryInstruction() const {
    return GetFrameFlag(FrameFlags::kForceRetryInst);
  }

  void SetForceRetryInstruction(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kForceRetryInst);
  }

  bool GetSkipMethodExitEvents() const {
    return GetFrameFlag(FrameFlags::kSkipMethodExitEvents);
  }

  void SetSkipMethodExitEvents(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kSkipMethodExitEvents);
  }

  bool GetSkipNextExceptionEvent() const {
    return GetFrameFlag(FrameFlags::kSkipNextExceptionEvent);
  }

  void SetSkipNextExceptionEvent(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kSkipNextExceptionEvent);
  }

  bool GetNotifyDexPcMoveEvents() const {
    return GetFrameFlag(FrameFlags::kNotifyDexPcMoveEvents);
  }

  void SetNotifyDexPcMoveEvents(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kNotifyDexPcMoveEvents);
  }

  bool GetNotifyExceptionHandledEvent() const {
    return GetFrameFlag(FrameFlags::kNotifyExceptionHandledEvent);
  }

  void SetNotifyExceptionHandledEvent(bool enable) {
    UpdateFrameFlag(enable, FrameFlags::kNotifyExceptionHandledEvent);
  }

  void CheckConsistentVRegs() const {
    if (kIsDebugBuild) {
      // A shadow frame visible to GC requires the following rule: for a given vreg,
      // its vreg reference equivalent should be the same, or null.
      for (uint32_t i = 0; i < NumberOfVRegs(); ++i) {
        int32_t reference_value = References()[i].AsVRegValue();
        CHECK((GetVReg(i) == reference_value) || (reference_value == 0));
      }
    }
  }

 private:
  ShadowFrame(uint32_t num_vregs, ArtMethod* method, uint32_t dex_pc)
      : link_(nullptr),
        method_(method),
        number_of_vregs_(num_vregs),
        dex_pc_(dex_pc),
        frame_flags_(0) {
    memset(vregs_, 0, num_vregs * (sizeof(uint32_t) + sizeof(StackReference<mirror::Object>)));
  }

  void UpdateFrameFlag(bool enable, FrameFlags flag) {
    if (enable) {
      frame_flags_ |= static_cast<uint32_t>(flag);
    } else {
      frame_flags_ &= ~static_cast<uint32_t>(flag);
    }
  }

  bool GetFrameFlag(FrameFlags flag) const {
    return (frame_flags_ & static_cast<uint32_t>(flag)) != 0;
  }

  const StackReference<mirror::Object>* References() const {
    const uint32_t* vreg_end = &vregs_[NumberOfVRegs()];
    return reinterpret_cast<const StackReference<mirror::Object>*>(vreg_end);
  }

  StackReference<mirror::Object>* References() {
    return const_cast<StackReference<mirror::Object>*>(
        const_cast<const ShadowFrame*>(this)->References());
  }

  // Link to previous shadow frame or null.
  ShadowFrame* link_;
  ArtMethod* method_;
  LockCountData lock_count_data_;  // This may contain GC roots when lock counting is active.
  const uint32_t number_of_vregs_;
  uint32_t dex_pc_;

  // This is a set of ShadowFrame::FrameFlags which denote special states this frame is in.
  // NB alignment requires that this field takes 4 bytes no matter its size. Only 7 bits are
  // currently used.
  uint32_t frame_flags_;

  // This is a two-part array:
  //  - [0..number_of_vregs) holds the raw virtual registers, and each element here is always 4
  //    bytes.
  //  - [number_of_vregs..number_of_vregs*2) holds only reference registers. Each element here is
  //    ptr-sized.
  // In other words when a primitive is stored in vX, the second (reference) part of the array will
  // be null. When a reference is stored in vX, the second (reference) part of the array will be a
  // copy of vX.
  uint32_t vregs_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(ShadowFrame);
};

struct ShadowFrameDeleter {
  inline void operator()(ShadowFrame* frame) {
    if (frame != nullptr) {
      frame->~ShadowFrame();
    }
  }
};

}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_SHADOW_FRAME_H_
