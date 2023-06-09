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
  # In order to test deoptimizing at quick-to-interpreter bridge,
  # we want to run in debuggable mode with jit compilation.
  # We also bump up the jit threshold to 10000 to make sure that the method
  # that should be interpreted is not compiled.
  ctx.default_run(
      args,
      jit=True,
      runtime_option=["-Xjitthreshold:10000"],
      Xcompiler_option=["--debuggable"])
