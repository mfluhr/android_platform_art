#!/bin/bash
#
# Copyright 2017 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


def run(ctx, args):
  # Make sure we call 'sync'
  # before executing dalvikvm because otherwise
  # it's highly likely the pushed JAR files haven't
  # been committed to permanent storage yet,
  # and when we mmap them the kernel will think
  # the memory is dirty (despite being file-backed).
  # (Note: this was reproducible 100% of the time on
  # a target angler device).
  ctx.default_run(args, sync=True)
