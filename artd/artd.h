/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef ART_ARTD_ARTD_H_
#define ART_ARTD_ARTD_H_

#include "aidl/com/android/server/art/BnArtd.h"
#include "android-base/result.h"
#include "android/binder_auto_utils.h"

namespace art {
namespace artd {

class Artd : public aidl::com::android::server::art::BnArtd {
 public:
  ndk::ScopedAStatus isAlive(bool* _aidl_return) override;

  android::base::Result<void> Start();
};

}  // namespace artd
}  // namespace art

#endif  // ART_ARTD_ARTD_H_
