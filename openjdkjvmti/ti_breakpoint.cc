/* Copyright (C) 2017 The Android Open Source Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file implements interfaces from the file jvmti.h. This implementation
 * is licensed under the same terms as the file jvmti.h.  The
 * copyright and license information for the file jvmti.h follows.
 *
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <functional>

#include "ti_breakpoint.h"

#include "art_jvmti.h"
#include "art_method-inl.h"
#include "base/mutex-inl.h"
#include "base/pointer_size.h"
#include "deopt_manager.h"
#include "dex/dex_file_annotations.h"
#include "dex/modifiers.h"
#include "events-inl.h"
#include "jni/jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "runtime_callbacks.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "thread_list.h"
#include "ti_phase.h"

namespace openjdkjvmti {

class JvmtiBreakpointReflectionSource : public art::ReflectionSourceInfo {
 public:
  JvmtiBreakpointReflectionSource(size_t pc, art::ArtMethod* m)
      : art::ReflectionSourceInfo(art::ReflectionSourceType::kSourceMiscInternal),
        pc_(pc),
        m_(m) {}

  void Describe(std::ostream& os) const override REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::ReflectionSourceInfo::Describe(os);
    os << " jvmti Breakpoint Method=" << m_->PrettyMethod() << " PC=" << pc_;
  }

 private:
  size_t pc_;
  art::ArtMethod* m_;
};

class BreakpointReflectiveValueCallback : public art::ReflectiveValueVisitCallback {
 public:
  void VisitReflectiveTargets(art::ReflectiveValueVisitor* visitor)
      REQUIRES(art::Locks::mutator_lock_) {
    art::Thread* self = art::Thread::Current();
    eh_->ForEachEnv(self, [&](ArtJvmTiEnv* env) NO_THREAD_SAFETY_ANALYSIS {
      art::Locks::mutator_lock_->AssertExclusiveHeld(self);
      art::WriterMutexLock mu(self, env->event_info_mutex_);
      std::vector<std::pair<Breakpoint, Breakpoint>> updated_breakpoints;
      for (auto it : env->breakpoints) {
        art::ArtMethod* orig_method = it.GetMethod();
        art::ArtMethod* am = visitor->VisitMethod(
            orig_method, JvmtiBreakpointReflectionSource(it.GetLocation(), orig_method));
        if (am != orig_method) {
          updated_breakpoints.push_back({ Breakpoint { am, it.GetLocation() }, it });
        }
      }
      for (auto it : updated_breakpoints) {
        DCHECK(env->breakpoints.find(it.second) != env->breakpoints.end());
        env->breakpoints.erase(it.second);
        env->breakpoints.insert(it.first);
      }
    });
  }

  EventHandler* eh_;
};

static BreakpointReflectiveValueCallback gReflectiveValueCallback;
void BreakpointUtil::Register(EventHandler* eh) {
  gReflectiveValueCallback.eh_ = eh;
  art::ScopedThreadStateChange stsc(art::Thread::Current(),
                                    art::ThreadState::kWaitingForDebuggerToAttach);
  art::ScopedSuspendAll ssa("Add breakpoint reflective value visit callback");
  art::RuntimeCallbacks* callbacks = art::Runtime::Current()->GetRuntimeCallbacks();
  callbacks->AddReflectiveValueVisitCallback(&gReflectiveValueCallback);
}

void BreakpointUtil::Unregister() {
  art::ScopedThreadStateChange stsc(art::Thread::Current(),
                                    art::ThreadState::kWaitingForDebuggerToAttach);
  art::ScopedSuspendAll ssa("Remove reflective value visit callback");
  art::RuntimeCallbacks* callbacks = art::Runtime::Current()->GetRuntimeCallbacks();
  callbacks->RemoveReflectiveValueVisitCallback(&gReflectiveValueCallback);
}

size_t Breakpoint::hash() const {
  return std::hash<uintptr_t> {}(reinterpret_cast<uintptr_t>(method_))
      ^ std::hash<jlocation> {}(location_);
}

Breakpoint::Breakpoint(art::ArtMethod* m, jlocation loc) : method_(m), location_(loc) {
  DCHECK(!m->IsDefault() || !m->IsCopied() || !m->IsInvokable())
      << "Flags are: 0x" << std::hex << m->GetAccessFlags();
}

void BreakpointUtil::RemoveBreakpointsInClass(ArtJvmTiEnv* env, art::mirror::Class* klass) {
  std::vector<Breakpoint> to_remove;
  {
    art::WriterMutexLock lk(art::Thread::Current(), env->event_info_mutex_);
    for (const Breakpoint& b : env->breakpoints) {
      if (b.GetMethod()->GetDeclaringClass() == klass) {
        to_remove.push_back(b);
      }
    }
    for (const Breakpoint& b : to_remove) {
      auto it = env->breakpoints.find(b);
      DCHECK(it != env->breakpoints.end());
      env->breakpoints.erase(it);
    }
  }
  DeoptManager* deopt = DeoptManager::Get();
  for (const Breakpoint& b : to_remove) {
    // TODO It might be good to send these all at once instead.
    deopt->RemoveMethodBreakpoint(b.GetMethod());
  }
}

jvmtiError BreakpointUtil::SetBreakpoint(jvmtiEnv* jenv, jmethodID method, jlocation location) {
  ArtJvmTiEnv* env = ArtJvmTiEnv::AsArtJvmTiEnv(jenv);
  if (method == nullptr) {
    return ERR(INVALID_METHODID);
  }
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method)->GetCanonicalMethod();
  if (location < 0 || static_cast<uint32_t>(location) >=
      art_method->DexInstructions().InsnsSizeInCodeUnits()) {
    return ERR(INVALID_LOCATION);
  }
  DeoptManager::Get()->AddMethodBreakpoint(art_method);
  {
    art::WriterMutexLock lk(art::Thread::Current(), env->event_info_mutex_);
    auto res_pair = env->breakpoints.insert(/* Breakpoint */ {art_method, location});
    if (LIKELY(res_pair.second)) {
      return OK;
    }
  }
  // Didn't get inserted because it's already present!
  DeoptManager::Get()->RemoveMethodBreakpoint(art_method);
  return ERR(DUPLICATE);
}

jvmtiError BreakpointUtil::ClearBreakpoint(jvmtiEnv* jenv, jmethodID method, jlocation location) {
  ArtJvmTiEnv* env = ArtJvmTiEnv::AsArtJvmTiEnv(jenv);
  if (method == nullptr) {
    return ERR(INVALID_METHODID);
  }
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method)->GetCanonicalMethod();
  {
    art::WriterMutexLock lk(art::Thread::Current(), env->event_info_mutex_);
    auto pos = env->breakpoints.find(/* Breakpoint */ {art_method, location});
    if (pos == env->breakpoints.end()) {
      return ERR(NOT_FOUND);
    }
    env->breakpoints.erase(pos);
  }
  DeoptManager::Get()->RemoveMethodBreakpoint(art_method);
  return OK;
}

}  // namespace openjdkjvmti
