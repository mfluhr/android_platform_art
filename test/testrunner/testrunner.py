#!/usr/bin/env python3
#
# [VPYTHON:BEGIN]
# python_version: "3.8"
# [VPYTHON:END]
#
# Copyright 2017, The Android Open Source Project
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

"""ART Run-Test TestRunner

The testrunner runs the ART run-tests by simply invoking the script.
It fetches the list of eligible tests from art/test directory, and list of
disabled tests from art/test/knownfailures.json. It runs the tests by
invoking art/test/run-test script and checks the exit value to decide if the
test passed or failed.

Before invoking the script, first build all the tests dependencies.
There are two major build targets for building target and host tests
dependencies:
1) test-art-host-run-test
2) test-art-target-run-test

There are various options to invoke the script which are:
-t: Either the test name as in art/test or the test name including the variant
    information. Eg, "-t 001-HelloWorld",
    "-t test-art-host-run-test-debug-prebuild-optimizing-relocate-ntrace-cms-checkjni-picimage-ndebuggable-no-jvmti-001-HelloWorld32"
-j: Number of thread workers to be used. Eg - "-j64"
--dry-run: Instead of running the test name, just print its name.
--verbose
-b / --build-dependencies: to build the dependencies before running the test

To specify any specific variants for the test, use --<<variant-name>>.
For eg, for compiler type as optimizing, use --optimizing.


In the end, the script will print the failed and skipped tests if any.

"""
import argparse
import collections

# b/140161314 diagnostics.
try:
  import concurrent.futures
except Exception:
  import sys
  sys.stdout.write("\n\n" + sys.executable + " " + sys.version + "\n\n")
  sys.stdout.flush()
  raise

import csv
import datetime
import fnmatch
import itertools
import json
import multiprocessing
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

import env
from target_config import target_config
from device_config import device_config
from typing import Dict, Set, List
from functools import lru_cache
from pathlib import Path

# TODO: make it adjustable per tests and for buildbots
#
# Note: this needs to be larger than run-test timeouts, as long as this script
#       does not push the value to run-test. run-test is somewhat complicated:
#                      base: 25m  (large for ASAN)
#        + timeout handling:  2m
#        +   gcstress extra: 20m
#        -----------------------
#                            47m
timeout = 3600 # 60 minutes

if env.ART_TEST_RUN_ON_ARM_FVP:
  # Increase timeout to 600 minutes due to the emulation overhead on FVP.
  timeout = 36000

# DISABLED_TEST_CONTAINER holds information about the disabled tests. It is a map
# that has key as the test name (like 001-HelloWorld), and value as set of
# variants that the test is disabled for.
DISABLED_TEST_CONTAINER = {}

# The Dict contains the list of all possible variants for a given type. For example,
# for key TARGET, the value would be target and host. The list is used to parse
# the test name given as the argument to run.
VARIANT_TYPE_DICT: Dict[str, Set[str]] = {}

# The set of all variant sets that are incompatible and will always be skipped.
NONFUNCTIONAL_VARIANT_SETS = set()

# The set contains all the variants of each time.
TOTAL_VARIANTS_SET: Set[str] = set()

# The colors are used in the output. When a test passes, COLOR_PASS is used,
# and so on.
COLOR_ERROR = '\033[91m'
COLOR_PASS = '\033[92m'
COLOR_SKIP = '\033[93m'
COLOR_NORMAL = '\033[0m'

# The set contains the list of all the possible run tests that are in art/test
# directory.
RUN_TEST_SET = set()

failed_tests = []
skipped_tests = []

# Flags
n_thread = 0
total_test_count = 0
verbose = False
dry_run = False
ignore_skips = False
build = False
dist = False
gdb = False
gdb_arg = ''
dump_cfg = ''
gdb_dex2oat = False
gdb_dex2oat_args = ''
csv_result = None
csv_writer = None
runtime_option = ''
with_agent: List[str] = []
run_test_option: List[str] = []
dex2oat_jobs = -1   # -1 corresponds to default threads for dex2oat
run_all_configs = False

# Dict containing extra arguments
extra_arguments: Dict[str, List[str]] = { "host" : [], "target" : [] }

# Dict to store user requested test variants.
# key: variant_type.
# value: set of variants user wants to run of type <key>.
_user_input_variants: collections.defaultdict = collections.defaultdict(set)


class ChildProcessTracker(object):
  """Keeps track of forked child processes to be able to kill them."""

  def __init__(self):
    self.procs = {}             # dict from pid to subprocess.Popen object
    self.mutex = threading.Lock()

  def wait(self, proc, timeout):
    """Waits on the given subprocess and makes it available to kill_all meanwhile.

    Args:
      proc: The subprocess.Popen object to wait on.
      timeout: Timeout passed on to proc.communicate.

    Returns: A tuple of the process stdout output and its return value.
    """
    with self.mutex:
      if self.procs is not None:
        self.procs[proc.pid] = proc
      else:
        os.killpg(proc.pid, signal.SIGKILL) # kill_all has already been called.
    try:
      output = proc.communicate(timeout=timeout)[0]
      return_value = proc.wait()
      return output, return_value
    finally:
      with self.mutex:
        if self.procs is not None:
          del self.procs[proc.pid]

  def kill_all(self):
    """Kills all currently running processes and any future ones."""
    with self.mutex:
      for pid in self.procs:
        os.killpg(pid, signal.SIGKILL)
      self.procs = None # Make future wait() calls kill their processes immediately.

child_process_tracker = ChildProcessTracker()

def setup_csv_result():
  """Set up the CSV output if required."""
  global csv_writer
  csv_writer = csv.writer(csv_result)
  # Write the header.
  csv_writer.writerow(['target', 'run', 'prebuild', 'compiler', 'relocate', 'trace', 'gc',
                       'jni', 'image', 'debuggable', 'jvmti', 'test', 'address_size', 'result'])


def send_csv_result(test, result):
  """
  Write a line into the CSV results file if one is available.
  """
  if csv_writer is not None:
    csv_writer.writerow(extract_test_name(test) + [result])

def close_csv_file():
  global csv_result
  global csv_writer
  if csv_result is not None:
    csv_writer = None
    csv_result.flush()
    csv_result.close()
    csv_result = None

def gather_test_info():
  """The method gathers test information about the test to be run which includes
  generating the list of total tests from the art/test directory and the list
  of disabled test. It also maps various variants to types.
  """
  global TOTAL_VARIANTS_SET
  # TODO: Avoid duplication of the variant names in different lists.
  VARIANT_TYPE_DICT['run'] = {'ndebug', 'debug'}
  VARIANT_TYPE_DICT['target'] = {'target', 'host', 'jvm'}
  VARIANT_TYPE_DICT['trace'] = {'trace', 'ntrace', 'stream'}
  VARIANT_TYPE_DICT['image'] = {'picimage', 'no-image'}
  VARIANT_TYPE_DICT['debuggable'] = {'ndebuggable', 'debuggable'}
  VARIANT_TYPE_DICT['gc'] = {'gcstress', 'gcverify', 'cms'}
  VARIANT_TYPE_DICT['prebuild'] = {'no-prebuild', 'prebuild'}
  VARIANT_TYPE_DICT['relocate'] = {'relocate', 'no-relocate'}
  VARIANT_TYPE_DICT['jni'] = {'jni', 'forcecopy', 'checkjni'}
  VARIANT_TYPE_DICT['address_sizes'] = {'64', '32'}
  VARIANT_TYPE_DICT['jvmti'] = {'no-jvmti', 'jvmti-stress', 'redefine-stress', 'trace-stress',
                                'field-stress', 'step-stress'}
  VARIANT_TYPE_DICT['compiler'] = {'interp-ac', 'interpreter', 'jit', 'jit-on-first-use',
                                   'optimizing', 'speed-profile', 'baseline'}

  for v_type in VARIANT_TYPE_DICT:
    TOTAL_VARIANTS_SET = TOTAL_VARIANTS_SET.union(VARIANT_TYPE_DICT.get(v_type))

  test_dir = env.ANDROID_BUILD_TOP + '/art/test'
  for f in os.listdir(test_dir):
    if fnmatch.fnmatch(f, '[0-9]*'):
      RUN_TEST_SET.add(f)


def setup_test_env():
  """The method sets default value for the various variants of the tests if they
  are already not set.
  """
  if env.ART_TEST_BISECTION:
    env.ART_TEST_RUN_TEST_NO_PREBUILD = True
    env.ART_TEST_RUN_TEST_PREBUILD = False
    # Bisection search writes to standard output.
    env.ART_TEST_QUIET = False

  global _user_input_variants
  global run_all_configs
  # These are the default variant-options we will use if nothing in the group is specified.
  default_variants = {
      'target': {'host', 'target'},
      'prebuild': {'prebuild'},
      'jvmti': { 'no-jvmti'},
      'compiler': {'optimizing',
                   'jit',
                   'interpreter',
                   'interp-ac',
                   'speed-profile'},
      'relocate': {'no-relocate'},
      'trace': {'ntrace'},
      'gc': {'cms'},
      'jni': {'checkjni'},
      'image': {'picimage'},
      'debuggable': {'ndebuggable'},
      'run': {'debug'},
      # address_sizes_target depends on the target so it is dealt with below.
  }
  # We want to pull these early since the full VARIANT_TYPE_DICT has a few additional ones we don't
  # want to pick up if we pass --all.
  default_variants_keys = default_variants.keys()
  if run_all_configs:
    default_variants = VARIANT_TYPE_DICT

  for key in default_variants_keys:
    if not _user_input_variants[key]:
      _user_input_variants[key] = default_variants[key]

  _user_input_variants['address_sizes_target'] = collections.defaultdict(set)
  if not _user_input_variants['address_sizes']:
    _user_input_variants['address_sizes_target']['target'].add(
        env.ART_PHONY_TEST_TARGET_SUFFIX)
    _user_input_variants['address_sizes_target']['host'].add(
        env.ART_PHONY_TEST_HOST_SUFFIX)
    if env.ART_TEST_RUN_TEST_2ND_ARCH:
      _user_input_variants['address_sizes_target']['host'].add(
          env.ART_2ND_PHONY_TEST_HOST_SUFFIX)
      _user_input_variants['address_sizes_target']['target'].add(
          env.ART_2ND_PHONY_TEST_TARGET_SUFFIX)
  else:
    _user_input_variants['address_sizes_target']['host'] = _user_input_variants['address_sizes']
    _user_input_variants['address_sizes_target']['target'] = _user_input_variants['address_sizes']

  global n_thread
  if 'target' in _user_input_variants['target']:
    device_name = get_device_name()
    if n_thread == 0:
      # Use only part of the cores since fully loading the device tends to lead to timeouts.
      fraction = 1.0 if env.ART_TEST_ON_VM else 0.75
      n_thread = max(1, int(get_target_cpu_count() * fraction))
  else:
    device_name = "host"
    if n_thread == 0:
      n_thread = get_host_cpu_count()
  print_text("Concurrency: {} ({})\n".format(n_thread, device_name))

  global extra_arguments
  for target in _user_input_variants['target']:
    extra_arguments[target] = find_extra_device_arguments(target)

  if not sys.stdout.isatty():
    global COLOR_ERROR
    global COLOR_PASS
    global COLOR_SKIP
    global COLOR_NORMAL
    COLOR_ERROR = ''
    COLOR_PASS = ''
    COLOR_SKIP = ''
    COLOR_NORMAL = ''

def find_extra_device_arguments(target):
  """
  Gets any extra arguments from the device_config.
  """
  device_name = target
  if target == 'target':
    device_name = get_device_name()
  return device_config.get(device_name, { 'run-test-args' : [] })['run-test-args']

def get_device_name():
  """
  Gets the value of ro.product.name from remote device (unless running on a VM).
  """
  if env.ART_TEST_RUN_FROM_SOONG:
    return "target"  # We can't use adb during build.
  if env.ART_TEST_ON_VM:
    return subprocess.Popen(f"{env.ART_SSH_CMD} uname -a".split(),
                            stdout = subprocess.PIPE,
                            universal_newlines=True).stdout.read().strip()

  proc = subprocess.Popen(['adb', 'shell', 'getprop', 'ro.product.name'],
                          stderr=subprocess.STDOUT,
                          stdout = subprocess.PIPE,
                          universal_newlines=True)
  # only wait 2 seconds.
  timeout_val = 2

  if env.ART_TEST_RUN_ON_ARM_FVP:
    # Increase timeout to 200 seconds due to the emulation overhead on FVP.
    timeout_val = 200

  output = proc.communicate(timeout = timeout_val)[0]
  success = not proc.wait()
  if success:
    return output.strip()
  else:
    print_text("Unable to determine device type!\n")
    print_text("Continuing anyway.\n")
    return "UNKNOWN_TARGET"

def run_tests(tests):
  """This method generates variants of the tests to be run and executes them.

  Args:
    tests: The set of tests to be run.
  """
  args_all = []

  # jvm does not run with all these combinations,
  # or at least it doesn't make sense for most of them.
  # TODO: support some jvm variants like jvmti ?
  target_input_variants = _user_input_variants['target']
  uncombinated_target_input_variants = []
  if 'jvm' in target_input_variants:
    _user_input_variants['target'].remove('jvm')
    uncombinated_target_input_variants.append('jvm')

  global total_test_count
  total_test_count = len(tests)
  if target_input_variants:
    for variant_type in VARIANT_TYPE_DICT:
      if not (variant_type == 'target' or 'address_sizes' in variant_type):
        total_test_count *= len(_user_input_variants[variant_type])
  target_address_combinations = 0
  for target in target_input_variants:
    for address_size in _user_input_variants['address_sizes_target'][target]:
      target_address_combinations += 1
  target_address_combinations += len(uncombinated_target_input_variants)
  total_test_count *= target_address_combinations

  if env.ART_TEST_WITH_STRACE:
    args_all += ['--strace']

  if env.ART_TEST_RUN_TEST_ALWAYS_CLEAN:
    args_all += ['--always-clean']

  if env.ART_TEST_BISECTION:
    args_all += ['--bisection-search']

  if gdb:
    args_all += ['--gdb']
    if gdb_arg:
      args_all += ['--gdb-arg', gdb_arg]

  if dump_cfg:
    args_all += ['--dump-cfg', dump_cfg]
  if gdb_dex2oat:
    args_all += ['--gdb-dex2oat']
    if gdb_dex2oat_args:
      args_all += ['--gdb-dex2oat-args', f'{gdb_dex2oat_args}']

  args_all += run_test_option

  if runtime_option:
    for opt in runtime_option:
      args_all += ['--runtime-option', opt]
  if with_agent:
    for opt in with_agent:
      args_all += ['--with-agent', opt]

  if dex2oat_jobs != -1:
    args_all += ['--dex2oat-jobs', str(dex2oat_jobs)]

  def iter_config(tests, input_variants, user_input_variants):
    config = itertools.product(tests, input_variants, user_input_variants['run'],
                                 user_input_variants['prebuild'], user_input_variants['compiler'],
                                 user_input_variants['relocate'], user_input_variants['trace'],
                                 user_input_variants['gc'], user_input_variants['jni'],
                                 user_input_variants['image'],
                                 user_input_variants['debuggable'], user_input_variants['jvmti'])
    return config

  # [--host, --target] combines with all the other user input variants.
  config = iter_config(tests, target_input_variants, _user_input_variants)
  # [--jvm] currently combines with nothing else. most of the extra flags we'd insert
  # would be unrecognizable by the 'java' binary, so avoid inserting any extra flags for now.
  uncombinated_config = iter_config(tests, uncombinated_target_input_variants, { 'run': [''],
      'prebuild': [''], 'compiler': [''],
      'relocate': [''], 'trace': [''],
      'gc': [''], 'jni': [''],
      'image': [''],
      'debuggable': [''], 'jvmti': ['']})

  def start_combination(executor, config_tuple, global_options, address_size):
      test, target, run, prebuild, compiler, relocate, trace, gc, \
      jni, image, debuggable, jvmti = config_tuple

      # NB The order of components here should match the order of
      # components in the regex parser in parse_test_name.
      test_name = 'test-art-'
      test_name += target + '-run-test-'
      test_name += run + '-'
      test_name += prebuild + '-'
      test_name += compiler + '-'
      test_name += relocate + '-'
      test_name += trace + '-'
      test_name += gc + '-'
      test_name += jni + '-'
      test_name += image + '-'
      test_name += debuggable + '-'
      test_name += jvmti + '-'
      test_name += test
      test_name += address_size

      variant_set = {target, run, prebuild, compiler, relocate, trace, gc, jni,
                     image, debuggable, jvmti, address_size}

      args_test = global_options.copy()

      if target == 'host':
        args_test += ['--host']
      elif target == 'jvm':
        args_test += ['--jvm']

      # Honor ART_TEST_CHROOT, ART_TEST_ANDROID_ROOT, ART_TEST_ANDROID_ART_ROOT,
      # ART_TEST_ANDROID_I18N_ROOT, and ART_TEST_ANDROID_TZDATA_ROOT but only
      # for target tests.
      if target == 'target':
        if env.ART_TEST_CHROOT:
          args_test += ['--chroot', env.ART_TEST_CHROOT]
        if env.ART_TEST_ANDROID_ROOT:
          args_test += ['--android-root', env.ART_TEST_ANDROID_ROOT]
        if env.ART_TEST_ANDROID_I18N_ROOT:
            args_test += ['--android-i18n-root', env.ART_TEST_ANDROID_I18N_ROOT]
        if env.ART_TEST_ANDROID_ART_ROOT:
          args_test += ['--android-art-root', env.ART_TEST_ANDROID_ART_ROOT]
        if env.ART_TEST_ANDROID_TZDATA_ROOT:
          args_test += ['--android-tzdata-root', env.ART_TEST_ANDROID_TZDATA_ROOT]

      if run == 'ndebug':
        args_test += ['-O']

      if prebuild == 'prebuild':
        args_test += ['--prebuild']
      elif prebuild == 'no-prebuild':
        args_test += ['--no-prebuild']

      if compiler == 'optimizing':
        args_test += ['--optimizing']
      elif compiler == 'interpreter':
        args_test += ['--interpreter']
      elif compiler == 'interp-ac':
        args_test += ['--switch-interpreter', '--verify-soft-fail']
      elif compiler == 'jit':
        args_test += ['--jit']
      elif compiler == 'jit-on-first-use':
        args_test += ['--jit', '--runtime-option', '-Xjitthreshold:0']
      elif compiler == 'speed-profile':
        args_test += ['--random-profile']
      elif compiler == 'baseline':
        args_test += ['--baseline']

      if relocate == 'relocate':
        args_test += ['--relocate']
      elif relocate == 'no-relocate':
        args_test += ['--no-relocate']

      if trace == 'trace':
        args_test += ['--trace']
      elif trace == 'stream':
        args_test += ['--trace', '--stream']

      if gc == 'gcverify':
        args_test += ['--gcverify']
      elif gc == 'gcstress':
        args_test += ['--gcstress']

      if jni == 'forcecopy':
        args_test += ['--runtime-option', '-Xjniopts:forcecopy']
      elif jni == 'checkjni':
        args_test += ['--runtime-option', '-Xcheck:jni']

      if image == 'no-image':
        args_test += ['--no-image']

      if debuggable == 'debuggable':
        args_test += ['--debuggable', '--runtime-option', '-Xopaque-jni-ids:true']

      if jvmti == 'jvmti-stress':
        args_test += ['--jvmti-trace-stress', '--jvmti-redefine-stress', '--jvmti-field-stress']
      elif jvmti == 'field-stress':
        args_test += ['--jvmti-field-stress']
      elif jvmti == 'trace-stress':
        args_test += ['--jvmti-trace-stress']
      elif jvmti == 'redefine-stress':
        args_test += ['--jvmti-redefine-stress']
      elif jvmti == 'step-stress':
        args_test += ['--jvmti-step-stress']

      if address_size == '64':
        args_test += ['--64']

      # Run the run-test script using the prebuilt python.
      python3_bin = env.ANDROID_BUILD_TOP + "/prebuilts/build-tools/path/linux-x86/python3"
      run_test_sh = str(Path(__file__).parent.parent / 'run-test')
      if not os.path.exists(python3_bin):
        python3_bin = sys.executable  # Fallback to current python if we are in a sandbox.
      args_test = [python3_bin, run_test_sh] + args_test + extra_arguments[target] + [test]
      return executor.submit(run_test, args_test, test, variant_set, test_name)

  global n_thread
  with concurrent.futures.ThreadPoolExecutor(max_workers=n_thread) as executor:
    test_futures = []
    for config_tuple in config:
      target = config_tuple[1]
      for address_size in _user_input_variants['address_sizes_target'][target]:
        test_futures.append(start_combination(executor, config_tuple, args_all, address_size))

    for config_tuple in uncombinated_config:
      test_futures.append(
          start_combination(executor, config_tuple, args_all, ""))  # no address size

    try:
      tests_done = 0
      for test_future in concurrent.futures.as_completed(f for f in test_futures if f):
        (test, status, failure_info, test_time) = test_future.result()
        tests_done += 1
        print_test_info(tests_done, test, status, failure_info, test_time)
        if failure_info and not env.ART_TEST_KEEP_GOING:
          for f in test_futures:
            f.cancel()
          break
    except KeyboardInterrupt:
      for f in test_futures:
        f.cancel()
      child_process_tracker.kill_all()
    executor.shutdown(True)

def _popen(**kwargs):
  if sys.version_info.major == 3 and sys.version_info.minor >= 6:
    return subprocess.Popen(encoding=sys.stdout.encoding, **kwargs)
  return subprocess.Popen(**kwargs)

def run_test(args, test, test_variant, test_name):
  """Runs the test.

  It invokes art/test/run-test script to run the test. The output of the script
  is checked, and if it ends with "Succeeded!", it assumes that the tests
  passed, otherwise, put it in the list of failed test. Before actually running
  the test, it also checks if the test is placed in the list of disabled tests,
  and if yes, it skips running it, and adds the test in the list of skipped
  tests.

  Args:
    args: The command to be used to invoke the script
    test: The name of the test without the variant information.
    test_variant: The set of variant for the test.
    test_name: The name of the test along with the variants.

  Returns: a tuple of testname, status, optional failure info, and test time.
  """
  try:
    command = ' '.join(args)

    if is_test_disabled(test, test_variant):
      test_skipped = True
      test_time = datetime.timedelta()
    else:
      test_skipped = False
      test_start_time = time.monotonic()
      if verbose:
        print_text("Starting %s at %s\n" % (test_name, test_start_time))
      environ = dict(os.environ)
      environ["FULL_TEST_NAME"] = test_name
      if gdb or gdb_dex2oat:
        proc = _popen(
          args=args,
          env=environ,
          stderr=subprocess.STDOUT,
          universal_newlines=True,
          start_new_session=True
        )
      else:
        proc = _popen(
          args=args,
          env=environ,
          stderr=subprocess.STDOUT,
          stdout = subprocess.PIPE,
          universal_newlines=True,
          start_new_session=True,
        )
      script_output, return_value = child_process_tracker.wait(proc, timeout)
      test_passed = not return_value
      test_time_seconds = time.monotonic() - test_start_time
      test_time = datetime.timedelta(seconds=test_time_seconds)

    if not test_skipped:
      if test_passed:
        return (test_name, 'PASS', None, test_time)
      else:
        failed_tests.append((test_name, str(command) + "\n" + script_output))
        return (test_name, 'FAIL', ('%s\n%s') % (command, script_output), test_time)
    elif not dry_run:
      skipped_tests.append(test_name)
      return (test_name, 'SKIP', None, test_time)
    else:
      return (test_name, 'PASS', None, test_time)
  except subprocess.TimeoutExpired as e:
    if verbose:
      print_text("Timeout of %s at %s\n" % (test_name, time.monotonic()))
    test_time_seconds = time.monotonic() - test_start_time
    test_time = datetime.timedelta(seconds=test_time_seconds)
    failed_tests.append((test_name, 'Timed out in %d seconds' % timeout))

    # HACK(b/142039427): Print extra backtraces on timeout.
    if "-target-" in test_name and not env.ART_TEST_ON_VM:
      for i in range(8):
        proc_name = "dalvikvm" + test_name[-2:]
        pidof = subprocess.run(["adb", "shell", "pidof", proc_name], stdout=subprocess.PIPE)
        for pid in pidof.stdout.decode("ascii").split():
          if i >= 4:
            print_text("Backtrace of %s at %s\n" % (pid, time.monotonic()))
            subprocess.run(["adb", "shell", "debuggerd", pid])
            time.sleep(10)
          task_dir = "/proc/%s/task" % pid
          tids = subprocess.run(["adb", "shell", "ls", task_dir], stdout=subprocess.PIPE)
          for tid in tids.stdout.decode("ascii").split():
            for status in ["stat", "status"]:
              filename = "%s/%s/%s" % (task_dir, tid, status)
              print_text("Content of %s\n" % (filename))
              subprocess.run(["adb", "shell", "cat", filename])
        time.sleep(60)

    # The python documentation states that it is necessary to actually kill the process.
    os.killpg(proc.pid, signal.SIGKILL)
    script_output = proc.communicate()

    return (test_name, 'TIMEOUT', 'Timed out in %d seconds\n%s' % (timeout, command), test_time)
  except Exception as e:
    failed_tests.append((test_name, str(e)))
    return (test_name, 'FAIL', ('%s\n%s\n\n') % (command, str(e)), datetime.timedelta())

@lru_cache
def get_console_width(default=100):
  # NB: The command may fail if we are running under 'nohup'.
  proc = subprocess.run(['stty', 'size'], capture_output=True)
  return int(proc.stdout.decode("utf8").split()[1]) if proc.returncode == 0 else default

def print_test_info(test_count, test_name, result, failed_test_info="",
                    test_time=datetime.timedelta()):
  """Print the continous test information

  If verbose is set to True, it continuously prints test status information
  on a new line.
  If verbose is set to False, it keeps on erasing test
  information by overriding it with the latest test information. Also,
  in this case it stictly makes sure that the information length doesn't
  exceed the console width. It does so by shortening the test_name.

  When a test fails, it prints the output of the run-test script and
  command used to invoke the script. It doesn't override the failing
  test information in either of the cases.
  """

  info = ''
  if not verbose:
    # Without --verbose, the testrunner erases passing test info. It
    # does that by overriding the printed text with white spaces all across
    # the console width.
    info = '\r' + ' ' * get_console_width() + '\r'
  try:
    percent = (test_count * 100) / total_test_count
    progress_info = ('[ %d%% %d/%d ]') % (
      percent,
      test_count,
      total_test_count)
    if test_time.total_seconds() != 0 and verbose:
      info += '(%s)' % str(test_time)


    if result == 'FAIL' or result == 'TIMEOUT':
      if not verbose:
        info += ('%s %s %s\n') % (
          progress_info,
          test_name,
          COLOR_ERROR + result + COLOR_NORMAL)
      else:
        info += ('%s %s %s\n%s\n') % (
          progress_info,
          test_name,
          COLOR_ERROR + result + COLOR_NORMAL,
          failed_test_info)
    else:
      result_text = ''
      if result == 'PASS':
        result_text += COLOR_PASS + 'PASS' + COLOR_NORMAL
      elif result == 'SKIP':
        result_text += COLOR_SKIP + 'SKIP' + COLOR_NORMAL

      if verbose:
        info += ('%s %s %s\n') % (
          progress_info,
          test_name,
          result_text)
      else:
        total_output_length = 2 # Two spaces
        total_output_length += len(progress_info)
        total_output_length += len(result)
        allowed_test_length = get_console_width() - total_output_length
        test_name_len = len(test_name)
        if allowed_test_length < test_name_len:
          test_name = ('...%s') % (
            test_name[-(allowed_test_length - 3):])
        info += ('%s %s %s') % (
          progress_info,
          test_name,
          result_text)
    send_csv_result(test_name, result)
    print_text(info)
  except Exception as e:
    print_text(('%s\n%s\n') % (test_name, str(e)))
    failed_tests.append(test_name)

def verify_knownfailure_entry(entry):
  supported_field = {
      'tests' : (list, str),
      'test_patterns' : (list,),
      'description' : (list, str),
      'bug' : (str,),
      'variant' : (str,),
      'devices': (list, str),
      'env_vars' : (dict,),
  }
  for field in entry:
    field_type = type(entry[field])
    if field_type not in supported_field[field]:
      raise ValueError('%s is not supported type for %s\n%s' % (
          str(field_type),
          field,
          str(entry)))

def get_disabled_test_info(device_name):
  """Generate set of known failures.

  It parses the art/test/knownfailures.json file to generate the list of
  disabled tests.

  Returns:
    The method returns a dict of tests mapped to the variants list
    for which the test should not be run.
  """
  known_failures_file = Path(__file__).parent.parent / 'knownfailures.json'
  with open(known_failures_file) as known_failures_json:
    known_failures_info = json.loads(known_failures_json.read())

  disabled_test_info = {}
  for failure in known_failures_info:
    verify_knownfailure_entry(failure)
    tests = failure.get('tests', [])
    if isinstance(tests, str):
      tests = [tests]
    patterns = failure.get("test_patterns", [])
    if (not isinstance(patterns, list)):
      raise ValueError("test_patterns is not a list in %s" % failure)

    tests += [f for f in RUN_TEST_SET if any(re.match(pat, f) is not None for pat in patterns)]
    variants = parse_variants(failure.get('variant'))

    # Treat a '"devices": "<foo>"' equivalent to 'target' variant if
    # "foo" is present in "devices".
    device_names = failure.get('devices', [])
    if isinstance(device_names, str):
      device_names = [device_names]
    if len(device_names) != 0:
      if device_name in device_names:
        variants.add('target')
      else:
        # Skip adding test info as device_name is not present in "devices" entry.
        continue

    env_vars = failure.get('env_vars')

    if check_env_vars(env_vars):
      for test in tests:
        if test not in RUN_TEST_SET:
          if env.ART_TEST_RUN_FROM_SOONG:
            continue  # Soong can see only sub-set of the tests within the shard.
          raise ValueError('%s is not a valid run-test' % (
              test))
        if test in disabled_test_info:
          disabled_test_info[test] = disabled_test_info[test].union(variants)
        else:
          disabled_test_info[test] = variants

  return disabled_test_info

def gather_disabled_test_info():
  global DISABLED_TEST_CONTAINER
  device_name = get_device_name() if 'target' in _user_input_variants['target'] else None
  DISABLED_TEST_CONTAINER = get_disabled_test_info(device_name)

def check_env_vars(env_vars):
  """Checks if the env variables are set as required to run the test.

  Returns:
    True if all the env variables are set as required, otherwise False.
  """

  if not env_vars:
    return True
  for key in env_vars:
    if env.get_env(key) != env_vars.get(key):
      return False
  return True


def is_test_disabled(test, variant_set):
  """Checks if the test along with the variant_set is disabled.

  Args:
    test: The name of the test as in art/test directory.
    variant_set: Variants to be used for the test.
  Returns:
    True, if the test is disabled.
  """
  if dry_run:
    return True
  if test in env.EXTRA_DISABLED_TESTS:
    return True
  if ignore_skips:
    return False
  variants_list = DISABLED_TEST_CONTAINER.get(test, {})
  for variants in variants_list:
    variants_present = True
    for variant in variants:
      if variant not in variant_set:
        variants_present = False
        break
    if variants_present:
      return True
  for bad_combo in NONFUNCTIONAL_VARIANT_SETS:
    if bad_combo.issubset(variant_set):
      return True
  return False


def parse_variants(variants):
  """Parse variants fetched from art/test/knownfailures.json.
  """
  if not variants:
    variants = ''
    for variant in TOTAL_VARIANTS_SET:
      variants += variant
      variants += '|'
    variants = variants[:-1]
  variant_list = set()
  or_variants = variants.split('|')
  for or_variant in or_variants:
    and_variants = or_variant.split('&')
    variant = set()
    for and_variant in and_variants:
      and_variant = and_variant.strip()
      if and_variant not in TOTAL_VARIANTS_SET:
        raise ValueError('%s is not a valid variant' % (
            and_variant))
      variant.add(and_variant)
    variant_list.add(frozenset(variant))
  return variant_list

def print_text(output, error=False):
  if env.ART_TEST_RUN_FROM_SOONG and not error:
    return  # Be quiet during build.
  sys.stdout.write(output)
  sys.stdout.flush()

def print_analysis():
  if not verbose:
    # Without --verbose, the testrunner erases passing test info. It
    # does that by overriding the printed text with white spaces all across
    # the console width.
    eraser_text = '\r' + ' ' * get_console_width() + '\r'
    print_text(eraser_text)

  # Prints information about the total tests run.
  # E.g., "2/38 (5%) tests passed".
  passed_test_count = total_test_count - len(skipped_tests) - len(failed_tests)
  passed_test_information = ('%d/%d (%d%%) %s passed.\n') % (
      passed_test_count,
      total_test_count,
      (passed_test_count*100)/total_test_count,
      'tests' if passed_test_count > 1 else 'test')
  print_text(passed_test_information)

  # Prints the list of skipped tests, if any.
  if skipped_tests:
    print_text(COLOR_SKIP + 'SKIPPED TESTS: ' + COLOR_NORMAL + '\n')
    for test in skipped_tests:
      print_text(test + '\n')
    print_text('\n')

  # Prints the list of failed tests, if any.
  if failed_tests:
    print_text(COLOR_ERROR + 'FAILED: ' + COLOR_NORMAL + '\n', error=True)
    for test_info in failed_tests:
      print_text(('%s\n%s\n' % (test_info[0], test_info[1])), error=True)
    print_text(COLOR_ERROR + '----------' + COLOR_NORMAL + '\n')
    for failed_test in sorted([test_info[0] for test_info in failed_tests]):
      print_text(('%s\n' % (failed_test)))

test_name_matcher = None
def extract_test_name(test_name):
  """Parses the test name and returns all the parts"""
  global test_name_matcher
  if test_name_matcher is None:
    regex = '^test-art-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['target']) + ')-'
    regex += 'run-test-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['run']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['prebuild']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['compiler']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['relocate']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['trace']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['gc']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['jni']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['image']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['debuggable']) + ')-'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['jvmti']) + ')-'
    regex += '(' + '|'.join(RUN_TEST_SET) + ')'
    regex += '(' + '|'.join(VARIANT_TYPE_DICT['address_sizes']) + ')$'
    test_name_matcher = re.compile(regex)
  match = test_name_matcher.match(test_name)
  if match:
    return list(match.groups())
  raise ValueError(test_name + " is not a valid test")

def parse_test_name(test_name):
  """Parses the testname provided by the user.
  It supports two types of test_name:
  1) Like 001-HelloWorld. In this case, it will just verify if the test actually
  exists and if it does, it returns the testname.
  2) Like test-art-host-run-test-debug-prebuild-interpreter-no-relocate-ntrace-cms-checkjni-picimage-ndebuggable-no-jvmti-001-HelloWorld32
  In this case, it will parse all the variants and check if they are placed
  correctly. If yes, it will set the various VARIANT_TYPES to use the
  variants required to run the test. Again, it returns the test_name
  without the variant information like 001-HelloWorld.
  """
  test_set = set()
  for test in RUN_TEST_SET:
    if test.startswith(test_name):
      test_set.add(test)
  if test_set:
    return test_set

  parsed = extract_test_name(test_name)
  _user_input_variants['target'].add(parsed[0])
  _user_input_variants['run'].add(parsed[1])
  _user_input_variants['prebuild'].add(parsed[2])
  _user_input_variants['compiler'].add(parsed[3])
  _user_input_variants['relocate'].add(parsed[4])
  _user_input_variants['trace'].add(parsed[5])
  _user_input_variants['gc'].add(parsed[6])
  _user_input_variants['jni'].add(parsed[7])
  _user_input_variants['image'].add(parsed[8])
  _user_input_variants['debuggable'].add(parsed[9])
  _user_input_variants['jvmti'].add(parsed[10])
  _user_input_variants['address_sizes'].add(parsed[12])
  return {parsed[11]}


def get_target_cpu_count():
  if env.ART_TEST_ON_VM:
    command = f"{env.ART_SSH_CMD} cat /sys/devices/system/cpu/present"
  else:
    command = 'adb shell cat /sys/devices/system/cpu/present'
  cpu_info_proc = subprocess.Popen(command.split(), stdout=subprocess.PIPE)
  cpu_info = cpu_info_proc.stdout.read()
  if type(cpu_info) is bytes:
    cpu_info = cpu_info.decode('utf-8')
  cpu_info_regex = r'\d*-(\d*)'
  match = re.match(cpu_info_regex, cpu_info)
  if match:
    return int(match.group(1)) + 1  # Add one to convert from "last-index" to "count"
  else:
    raise ValueError('Unable to predict the concurrency for the target. '
                     'Is device connected?')


def get_host_cpu_count():
  return multiprocessing.cpu_count()


def parse_option():
  global verbose
  global dry_run
  global ignore_skips
  global n_thread
  global build
  global dist
  global gdb
  global gdb_arg
  global dump_cfg
  global gdb_dex2oat
  global gdb_dex2oat_args
  global runtime_option
  global run_test_option
  global timeout
  global dex2oat_jobs
  global run_all_configs
  global with_agent
  global csv_result

  parser = argparse.ArgumentParser(description="Runs all or a subset of the ART test suite.")
  parser.add_argument('tests', action='extend', nargs="*", help='name(s) of the test(s)')
  parser.add_argument('-t', '--test', action='append', dest='tests', help='name(s) of the test(s)'
      ' (deprecated: use positional arguments at the end without any option instead)')
  global_group = parser.add_argument_group('Global options',
                                           'Options that affect all tests being run')
  global_group.add_argument('-j', type=int, dest='n_thread', help="""Number of CPUs to use.
                            Defaults to half of CPUs on target and all CPUs on host.""")
  global_group.add_argument('--timeout', default=timeout, type=int, dest='timeout')
  global_group.add_argument('--verbose', '-v', action='store_true', dest='verbose')
  global_group.add_argument('--dry-run', action='store_true', dest='dry_run')
  global_group.add_argument("--skip", action='append', dest="skips", default=[],
                            help="Skip the given test in all circumstances.")
  global_group.add_argument("--no-skips", dest="ignore_skips", action='store_true', default=False,
                            help="""Don't skip any run-test configurations listed in
                            knownfailures.json.""")
  global_group.add_argument('--no-build-dependencies',
                            action='store_false', dest='build',
                            help="""Don't build dependencies under any circumstances. This is the
                            behavior if ART_TEST_RUN_TEST_ALWAYS_BUILD is not set to 'true'.""")
  global_group.add_argument('-b', '--build-dependencies',
                            action='store_true', dest='build',
                            help="""Build dependencies under all circumstances. By default we will
                            not build dependencies unless ART_TEST_RUN_TEST_BUILD=true.""")
  global_group.add_argument('--dist',
                            action='store_true', dest='dist',
                            help="""If dependencies are to be built, pass `dist` to the build
                            command line. You may want to also set the DIST_DIR environment
                            variable when using this flag.""")
  global_group.set_defaults(build = env.ART_TEST_RUN_TEST_BUILD)
  global_group.add_argument('--gdb', action='store_true', dest='gdb')
  global_group.add_argument('--gdb-arg', dest='gdb_arg')
  global_group.add_argument('--dump-cfg', dest='dump_cfg',
                            help="""Dump the CFG to the specified host path.
                            Example \"--dump-cfg <full-path>/graph.cfg\".""")
  global_group.add_argument('--gdb-dex2oat', action='store_true', dest='gdb_dex2oat')
  global_group.add_argument('--gdb-dex2oat-args', dest='gdb_dex2oat_args')
  global_group.add_argument('--run-test-option', action='append', dest='run_test_option',
                            default=[],
                            help="""Pass an option, unaltered, to the run-test script.
                            This should be enclosed in single-quotes to allow for spaces. The option
                            will be split using shlex.split() prior to invoking run-test.
                            Example \"--run-test-option='--with-agent libtifast.so=MethodExit'\".""")
  global_group.add_argument('--with-agent', action='append', dest='with_agent',
                            help="""Pass an agent to be attached to the runtime""")
  global_group.add_argument('--runtime-option', action='append', dest='runtime_option',
                            help="""Pass an option to the runtime. Runtime options
                            starting with a '-' must be separated by a '=', for
                            example '--runtime-option=-Xjitthreshold:0'.""")
  global_group.add_argument('--dex2oat-jobs', type=int, dest='dex2oat_jobs',
                            help='Number of dex2oat jobs')
  global_group.add_argument('-a', '--all', action='store_true', dest='run_all',
                            help="Run all the possible configurations for the input test set")
  global_group.add_argument('--csv-results', action='store', dest='csv_result', default=None,
                            type=argparse.FileType('w'), help='Store a CSV record of all results.')
  for variant_type, variant_set in VARIANT_TYPE_DICT.items():
    var_group = parser.add_argument_group(
        '{}-type Options'.format(variant_type),
        "Options that control the '{}' variants.".format(variant_type))
    var_group.add_argument('--all-' + variant_type,
                           action='store_true',
                           dest='all_' + variant_type,
                           help='Enable all variants of ' + variant_type)
    for variant in variant_set:
      flag = '--' + variant
      var_group.add_argument(flag, action='store_true', dest=variant)

  options = vars(parser.parse_args())
  if options['csv_result'] is not None:
    csv_result = options['csv_result']
    setup_csv_result()
  # Handle the --all-<type> meta-options
  for variant_type, variant_set in VARIANT_TYPE_DICT.items():
    if options['all_' + variant_type]:
      for variant in variant_set:
        options[variant] = True

  tests = None
  env.EXTRA_DISABLED_TESTS.update(set(options['skips']))
  if options['tests']:
    tests = set()
    for test_name in options['tests']:
      tests |= parse_test_name(test_name)

  for variant_type in VARIANT_TYPE_DICT:
    for variant in VARIANT_TYPE_DICT[variant_type]:
      if options.get(variant):
        _user_input_variants[variant_type].add(variant)

  if options['verbose']:
    verbose = True
  if options['n_thread']:
    n_thread = max(1, options['n_thread'])
  ignore_skips = options['ignore_skips']
  if options['dry_run']:
    dry_run = True
    verbose = True
  build = options['build']
  dist = options['dist']
  if options['gdb']:
    n_thread = 1
    gdb = True
    if options['gdb_arg']:
      gdb_arg = options['gdb_arg']
  if options['dump_cfg']:
    dump_cfg = options['dump_cfg']
  if options['gdb_dex2oat']:
    n_thread = 1
    gdb_dex2oat = True
    if options['gdb_dex2oat_args']:
      gdb_dex2oat_args = options['gdb_dex2oat_args']
  runtime_option = options['runtime_option'];
  with_agent = options['with_agent'];
  run_test_option = sum(map(shlex.split, options['run_test_option']), [])

  timeout = options['timeout']
  if options['dex2oat_jobs']:
    dex2oat_jobs = options['dex2oat_jobs']
  if options['run_all']:
    run_all_configs = True

  return tests or RUN_TEST_SET

def main():
  gather_test_info()
  tests = parse_option()
  setup_test_env()
  gather_disabled_test_info()
  if build:
    build_targets = []
    # Build only the needed shards (depending on the selected tests).
    shards = set(re.search(r"(\d\d)-", t).group(1) for t in tests)
    if any("hiddenapi" in t for t in tests):
      shards.add("HiddenApi")  # Include special HiddenApi shard.
    for mode in ['host', 'target', 'jvm']:
      if mode in _user_input_variants['target']:
        build_targets += ['test-art-{}-run-test-dependencies'.format(mode)]
        if len(shards) >= 100:
          build_targets += ["art-run-test-{}-data".format(mode)]  # Build all.
        else:
          build_targets += ["art-run-test-{}-data-shard{}".format(mode, s) for s in shards]
    build_command = env.ANDROID_BUILD_TOP + '/build/soong/soong_ui.bash --make-mode'
    build_command += ' D8='
    if dist:
      build_command += ' dist'
    build_command += ' ' + ' '.join(build_targets)
    print_text('Build command: %s\n' % build_command)
    if subprocess.call(build_command.split()):
      # Debugging for b/62653020
      if env.DIST_DIR:
        shutil.copyfile(env.SOONG_OUT_DIR + '/build.ninja', env.DIST_DIR + '/soong.ninja')
      sys.exit(1)

  run_tests(tests)

  print_analysis()
  close_csv_file()

  exit_code = 0 if len(failed_tests) == 0 else 1
  sys.exit(exit_code)

if __name__ == '__main__':
  main()
