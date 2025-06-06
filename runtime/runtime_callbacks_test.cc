/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "runtime_callbacks.h"

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>

#include "jni.h"

#include "art_method-alloc-inl.h"
#include "base/mem_map.h"
#include "base/mutex.h"
#include "class_linker.h"
#include "common_runtime_test.h"
#include "dex/class_reference.h"
#include "handle.h"
#include "handle_scope-inl.h"
#include "instrumentation.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class_loader.h"
#include "monitor-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "obj_ptr-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "well_known_classes-inl.h"

namespace art HIDDEN {

class RuntimeCallbacksTest : public CommonRuntimeTest {
 protected:
  void SetUp() override {
    CommonRuntimeTest::SetUp();

    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    ScopedThreadSuspension sts(self, ThreadState::kWaitingForDebuggerToAttach);
    ScopedSuspendAll ssa("RuntimeCallbacksTest SetUp");
    AddListener();
  }

  void TearDown() override {
    {
      Thread* self = Thread::Current();
      ScopedObjectAccess soa(self);
      ScopedThreadSuspension sts(self, ThreadState::kWaitingForDebuggerToAttach);
      ScopedSuspendAll ssa("RuntimeCallbacksTest TearDown");
      RemoveListener();
    }

    CommonRuntimeTest::TearDown();
  }

  virtual void AddListener() REQUIRES(Locks::mutator_lock_) = 0;
  virtual void RemoveListener() REQUIRES(Locks::mutator_lock_) = 0;

  void MakeExecutable(ObjPtr<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_) {
    CHECK(klass != nullptr);
    PointerSize pointer_size = class_linker_->GetImagePointerSize();
    for (auto& m : klass->GetMethods(pointer_size)) {
      if (!m.IsAbstract()) {
        Runtime::Current()->GetInstrumentation()->ReinitializeMethodsCode(&m);
      }
    }
  }
};

class ThreadLifecycleCallbackRuntimeCallbacksTest : public RuntimeCallbacksTest {
 public:
  static void* PthreadsCallback([[maybe_unused]] void* arg) {
    // Attach.
    Runtime* runtime = Runtime::Current();
    CHECK(runtime->AttachCurrentThread("ThreadLifecycle test thread", true, nullptr, false));

    // Detach.
    runtime->DetachCurrentThread();

    // Die...
    return nullptr;
  }

 protected:
  void AddListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->AddThreadLifecycleCallback(&cb_);
  }
  void RemoveListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->RemoveThreadLifecycleCallback(&cb_);
  }

  enum CallbackState {
    kBase,
    kStarted,
    kDied,
    kWrongStart,
    kWrongDeath,
  };

  struct Callback : public ThreadLifecycleCallback {
    void ThreadStart(Thread* self) override {
      {
        ScopedObjectAccess soa(self);
        LOG(DEBUG) << "ThreadStart callback for thread: " << self->GetThreadName();
      }
      if (state == CallbackState::kBase) {
        state = CallbackState::kStarted;
        stored_self = self;
      } else {
        state = CallbackState::kWrongStart;
      }
    }

    void ThreadDeath(Thread* self) override {
      {
        ScopedObjectAccess soa(self);
        LOG(DEBUG) << "ThreadDeath callback for thread: " << self->GetThreadName();
      }
      if (state == CallbackState::kStarted && self == stored_self) {
        state = CallbackState::kDied;
      } else {
        state = CallbackState::kWrongDeath;
      }
    }

    Thread* stored_self;
    CallbackState state = CallbackState::kBase;
  };

  Callback cb_;
};

TEST_F(ThreadLifecycleCallbackRuntimeCallbacksTest, ThreadLifecycleCallbackJava) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  bool started = runtime_->Start();
  ASSERT_TRUE(started);
  // Make sure the workers are done starting so we don't get callbacks for them.
  runtime_->WaitForThreadPoolWorkersToStart();

  // The metrics reporting thread will sometimes be slow to start. Synchronously requesting a
  // metrics report forces us to wait until the thread has started.
  runtime_->RequestMetricsReport(/*synchronous=*/true);

  cb_.state = CallbackState::kBase;  // Ignore main thread attach.

  ScopedObjectAccess soa(self);
  MakeExecutable(WellKnownClasses::java_lang_Thread.Get());

  StackHandleScope<3u> hs(self);
  Handle<mirror::String> thread_name = hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(self, "ThreadLifecycleCallback test thread"));
  ASSERT_TRUE(thread_name != nullptr);

  Handle<mirror::Object> thread_group =
      hs.NewHandle(soa.Decode<mirror::Object>(runtime_->GetMainThreadGroup()));
  Handle<mirror::Object> thread =
      WellKnownClasses::java_lang_Thread_init->NewObject<'L', 'L', 'I', 'Z'>(
          hs, self, thread_group, thread_name, kMinThreadPriority, /*daemon=*/ false);
  ASSERT_FALSE(self->IsExceptionPending());
  ASSERT_TRUE(thread != nullptr);

  ArtMethod* start_method =
      thread->GetClass()->FindClassMethod("start", "()V", kRuntimePointerSize);
  ASSERT_TRUE(start_method != nullptr);

  start_method->InvokeVirtual<'V'>(self, thread.Get());
  ASSERT_FALSE(self->IsExceptionPending());

  ArtMethod* join_method = thread->GetClass()->FindClassMethod("join", "()V", kRuntimePointerSize);
  ASSERT_TRUE(join_method != nullptr);

  join_method->InvokeFinal<'V'>(self, thread.Get());
  ASSERT_FALSE(self->IsExceptionPending());

  EXPECT_EQ(cb_.state, CallbackState::kDied);
}

TEST_F(ThreadLifecycleCallbackRuntimeCallbacksTest, ThreadLifecycleCallbackAttach) {
  std::string error_msg;
  MemMap stack = MemMap::MapAnonymous("ThreadLifecycleCallback Thread",
                                      128 * gPageSize,  // Just some small stack.
                                      PROT_READ | PROT_WRITE,
                                      /*low_4gb=*/ false,
                                      &error_msg);
  ASSERT_TRUE(stack.IsValid()) << error_msg;

  const char* reason = "ThreadLifecycleCallback test thread";
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), reason);
  CHECK_PTHREAD_CALL(pthread_attr_setstack, (&attr, stack.Begin(), stack.Size()), reason);
  pthread_t pthread;
  CHECK_PTHREAD_CALL(pthread_create,
                     (&pthread,
                         &attr,
                         &ThreadLifecycleCallbackRuntimeCallbacksTest::PthreadsCallback,
                         this),
                         reason);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), reason);

  CHECK_PTHREAD_CALL(pthread_join, (pthread, nullptr), "ThreadLifecycleCallback test shutdown");

  // Detach is not a ThreadDeath event, so we expect to be in state Started.
  EXPECT_TRUE(cb_.state == CallbackState::kStarted) << static_cast<int>(cb_.state);
}

class ClassLoadCallbackRuntimeCallbacksTest : public RuntimeCallbacksTest {
 protected:
  void AddListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->AddClassLoadCallback(&cb_);
  }
  void RemoveListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->RemoveClassLoadCallback(&cb_);
  }

  bool Expect(std::initializer_list<const char*> list) {
    if (cb_.data.size() != list.size()) {
      PrintError(list);
      return false;
    }

    if (!std::equal(cb_.data.begin(), cb_.data.end(), list.begin())) {
      PrintError(list);
      return false;
    }

    return true;
  }

  void PrintError(std::initializer_list<const char*> list) {
    LOG(ERROR) << "Expected:";
    for (const char* expected : list) {
      LOG(ERROR) << "  " << expected;
    }
    LOG(ERROR) << "Found:";
    for (const auto& s : cb_.data) {
      LOG(ERROR) << "  " << s;
    }
  }

  struct Callback : public ClassLoadCallback {
    void ClassPreDefine(const char* descriptor,
                        [[maybe_unused]] Handle<mirror::Class> klass,
                        [[maybe_unused]] Handle<mirror::ClassLoader> class_loader,
                        const DexFile& initial_dex_file,
                        [[maybe_unused]] const dex::ClassDef& initial_class_def,
                        [[maybe_unused]] /*out*/ DexFile const** final_dex_file,
                        [[maybe_unused]] /*out*/ dex::ClassDef const** final_class_def) override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      const std::string& location = initial_dex_file.GetLocation();
      std::string event =
          std::string("PreDefine:") + descriptor + " <" +
          location.substr(location.rfind('/') + 1, location.size()) + ">";
      data.push_back(event);
    }

    void ClassLoad(Handle<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
      std::string tmp;
      std::string event = std::string("Load:") + klass->GetDescriptor(&tmp);
      data.push_back(event);
    }

    void ClassPrepare(Handle<mirror::Class> temp_klass,
                      Handle<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
      std::string tmp, tmp2;
      std::string event = std::string("Prepare:") + klass->GetDescriptor(&tmp)
          + "[" + temp_klass->GetDescriptor(&tmp2) + "]";
      data.push_back(event);
    }

    std::vector<std::string> data;
  };

  Callback cb_;
};

TEST_F(ClassLoadCallbackRuntimeCallbacksTest, ClassLoadCallback) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  cb_.data.clear();  // Clear class loading records from `LoadDex()`, if any.
  VariableSizedHandleScope hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader>(jclass_loader)));

  const char* descriptor_y = "LY;";
  Handle<mirror::Class> h_Y = hs.NewHandle(FindClass(descriptor_y, class_loader));
  ASSERT_TRUE(h_Y != nullptr);

  bool expect1 = Expect({ "PreDefine:LY; <art-gtest-jars-XandY.jar>",
                          "PreDefine:LX; <art-gtest-jars-XandY.jar>",
                          "Load:LX;",
                          "Prepare:LX;[LX;]",
                          "Load:LY;",
                          "Prepare:LY;[LY;]" });
  EXPECT_TRUE(expect1);

  cb_.data.clear();

  ASSERT_TRUE(class_linker_->EnsureInitialized(Thread::Current(), h_Y, true, true));

  bool expect2 = Expect({ "PreDefine:LY$Z; <art-gtest-jars-XandY.jar>",
                          "Load:LY$Z;",
                          "Prepare:LY$Z;[LY$Z;]" });
  EXPECT_TRUE(expect2);
}

class RuntimeSigQuitCallbackRuntimeCallbacksTest : public RuntimeCallbacksTest {
 protected:
  void AddListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->AddRuntimeSigQuitCallback(&cb_);
  }
  void RemoveListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->RemoveRuntimeSigQuitCallback(&cb_);
  }

  struct Callback : public RuntimeSigQuitCallback {
    void SigQuit() override {
      ++sigquit_count;
    }

    size_t sigquit_count = 0;
  };

  Callback cb_;
};

TEST_F(RuntimeSigQuitCallbackRuntimeCallbacksTest, SigQuit) {
  // The runtime needs to be started for the signal handler.
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  bool started = runtime_->Start();
  ASSERT_TRUE(started);

  EXPECT_EQ(0u, cb_.sigquit_count);

  kill(getpid(), SIGQUIT);

  // Try a few times.
  for (size_t i = 0; i != 30; ++i) {
    if (cb_.sigquit_count == 0) {
      sleep(1);
    } else {
      break;
    }
  }
  EXPECT_EQ(1u, cb_.sigquit_count);
}

class RuntimePhaseCallbackRuntimeCallbacksTest : public RuntimeCallbacksTest {
 protected:
  void AddListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->AddRuntimePhaseCallback(&cb_);
  }
  void RemoveListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->RemoveRuntimePhaseCallback(&cb_);
  }

  void TearDown() override {
    // Bypass RuntimeCallbacksTest::TearDown, as the runtime is already gone.
    CommonRuntimeTest::TearDown();
  }

  struct Callback : public RuntimePhaseCallback {
    void NextRuntimePhase(RuntimePhaseCallback::RuntimePhase p) override {
      if (p == RuntimePhaseCallback::RuntimePhase::kInitialAgents) {
        if (start_seen > 0 || init_seen > 0 || death_seen > 0) {
          LOG(FATAL) << "Unexpected order";
        }
        ++initial_agents_seen;
      } else if (p == RuntimePhaseCallback::RuntimePhase::kStart) {
        if (init_seen > 0 || death_seen > 0) {
          LOG(FATAL) << "Init seen before start.";
        }
        ++start_seen;
      } else if (p == RuntimePhaseCallback::RuntimePhase::kInit) {
        ++init_seen;
      } else if (p == RuntimePhaseCallback::RuntimePhase::kDeath) {
        ++death_seen;
      } else {
        LOG(FATAL) << "Unknown phase " << static_cast<uint32_t>(p);
      }
    }

    size_t initial_agents_seen = 0;
    size_t start_seen = 0;
    size_t init_seen = 0;
    size_t death_seen = 0;
  };

  Callback cb_;
};

TEST_F(RuntimePhaseCallbackRuntimeCallbacksTest, Phases) {
  ASSERT_EQ(0u, cb_.initial_agents_seen);
  ASSERT_EQ(0u, cb_.start_seen);
  ASSERT_EQ(0u, cb_.init_seen);
  ASSERT_EQ(0u, cb_.death_seen);

  // Start the runtime.
  {
    Thread* self = Thread::Current();
    self->TransitionFromSuspendedToRunnable();
    bool started = runtime_->Start();
    ASSERT_TRUE(started);
  }

  ASSERT_EQ(0u, cb_.initial_agents_seen);
  ASSERT_EQ(1u, cb_.start_seen);
  ASSERT_EQ(1u, cb_.init_seen);
  ASSERT_EQ(0u, cb_.death_seen);

  // Delete the runtime.
  runtime_.reset();

  ASSERT_EQ(0u, cb_.initial_agents_seen);
  ASSERT_EQ(1u, cb_.start_seen);
  ASSERT_EQ(1u, cb_.init_seen);
  ASSERT_EQ(1u, cb_.death_seen);
}

class MonitorWaitCallbacksTest : public RuntimeCallbacksTest {
 protected:
  void AddListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->AddMonitorCallback(&cb_);
  }
  void RemoveListener() override REQUIRES(Locks::mutator_lock_) {
    Runtime::Current()->GetRuntimeCallbacks()->RemoveMonitorCallback(&cb_);
  }

  struct Callback : public MonitorCallback {
    bool IsInterestingObject(ObjPtr<mirror::Object> obj)
        REQUIRES_SHARED(art::Locks::mutator_lock_) {
      if (!obj->IsClass()) {
        return false;
      }
      std::lock_guard<std::mutex> lock(ref_guard_);
      ObjPtr<mirror::Class> k = obj->AsClass();
      ClassReference test = { &k->GetDexFile(), k->GetDexClassDefIndex() };
      return ref_ == test;
    }

    void SetInterestingObject(ObjPtr<mirror::Object> obj)
        REQUIRES_SHARED(art::Locks::mutator_lock_) {
      std::lock_guard<std::mutex> lock(ref_guard_);
      ObjPtr<mirror::Class> k = obj->AsClass();
      ref_ = { &k->GetDexFile(), k->GetDexClassDefIndex() };
    }

    void MonitorContendedLocking([[maybe_unused]] Monitor* mon) override
        REQUIRES_SHARED(Locks::mutator_lock_) {}

    void MonitorContendedLocked([[maybe_unused]] Monitor* mon) override
        REQUIRES_SHARED(Locks::mutator_lock_) {}

    void ObjectWaitStart(Handle<mirror::Object> obj, [[maybe_unused]] int64_t millis) override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      if (IsInterestingObject(obj.Get())) {
        saw_wait_start_ = true;
      }
    }

    void MonitorWaitFinished(Monitor* m, [[maybe_unused]] bool timed_out) override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      if (IsInterestingObject(m->GetObject())) {
        saw_wait_finished_ = true;
      }
    }

    std::mutex ref_guard_;
    ClassReference ref_ = {nullptr, 0};
    bool saw_wait_start_ = false;
    bool saw_wait_finished_ = false;
  };

  Callback cb_;
};

// TODO It would be good to have more tests for this but due to the multi-threaded nature of the
// callbacks this is difficult. For now the run-tests 1931 & 1932 should be sufficient.
TEST_F(MonitorWaitCallbacksTest, WaitUnlocked) {
  ASSERT_FALSE(cb_.saw_wait_finished_);
  ASSERT_FALSE(cb_.saw_wait_start_);
  {
    Thread* self = Thread::Current();
    self->TransitionFromSuspendedToRunnable();
    bool started = runtime_->Start();
    ASSERT_TRUE(started);
    {
      ScopedObjectAccess soa(self);
      cb_.SetInterestingObject(WellKnownClasses::java_util_Collections.Get());
      Monitor::Wait(
          self,
          // Just a random class
          WellKnownClasses::java_util_Collections.Get(),
          /*ms=*/0,
          /*ns=*/0,
          /*interruptShouldThrow=*/false,
          /*why=*/ThreadState::kWaiting);
    }
  }
  ASSERT_TRUE(cb_.saw_wait_start_);
  ASSERT_FALSE(cb_.saw_wait_finished_);
}

}  // namespace art
