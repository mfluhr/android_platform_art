#!/bin/bash
#
# Copyright (C) 2017 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


def run(ctx, args):
  # Let the test build its own core image with --no-image and use verify,
  # so that the compiler does not try to initialize classes. This leaves the
  # java.lang.Void compile-time verified but uninitialized.
  ctx.default_run(
      args,
      image=False,
      runtime_option=["-Ximage-compiler-option", "--compiler-filter=verify"])
