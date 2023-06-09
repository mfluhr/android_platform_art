#!/bin/bash
#
# Copyright (C) 2020 The Android Open Source Project
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
  # We need a profile to tell dex2oat to include classes in the final app image
  pcl = f"PCL[{ctx.env.DEX_LOCATION}/{ctx.env.TEST_NAME}.jar]"
  ctx.default_run(args, profile=True, secondary_class_loader_context=pcl)
