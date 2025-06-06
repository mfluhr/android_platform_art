/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_SIGNAL_SET_H_
#define ART_RUNTIME_SIGNAL_SET_H_

#include <signal.h>

#include <android-base/logging.h>

#include "base/macros.h"

#if defined(__GLIBC__) || defined(ANDROID_HOST_MUSL)
#define sigset64_t sigset_t
#define sigemptyset64 sigemptyset
#define sigaddset64 sigaddset
#define pthread_sigmask64 pthread_sigmask
#define sigwait64 sigwait
#define sigwaitinfo64 sigwaitinfo
#endif

namespace art HIDDEN {

class SignalSet {
 public:
  SignalSet() {
    if (sigemptyset64(&set_) == -1) {
      PLOG(FATAL) << "sigemptyset failed";
    }
  }

  void Add(int signal) {
    if (sigaddset64(&set_, signal) == -1) {
      PLOG(FATAL) << "sigaddset " << signal << " failed";
    }
  }

  void Block() {
    if (pthread_sigmask64(SIG_BLOCK, &set_, nullptr) != 0) {
      PLOG(FATAL) << "pthread_sigmask failed";
    }
  }

  int Wait(siginfo_t* info) {
    // Sleep in sigwait() until a signal arrives. gdb causes EINTR failures.
    while (true) {
      int signal_number = TEMP_FAILURE_RETRY(sigwaitinfo64(&set_, info));
      if (signal_number > 0) {
        return signal_number;
      }
      PLOG(FATAL) << "sigwaitinfo failed";
    }
  }

 private:
  sigset64_t set_;
};

}  // namespace art

#endif  // ART_RUNTIME_SIGNAL_SET_H_
