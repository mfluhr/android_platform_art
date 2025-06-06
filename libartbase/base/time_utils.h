/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_LIBARTBASE_BASE_TIME_UTILS_H_
#define ART_LIBARTBASE_BASE_TIME_UTILS_H_

#ifdef _WIN32
#include <stdio.h>  // Needed for correct macro definitions.
#endif

#include <time.h>

#include <cstdint>
#include <string>

#include "android-base/logging.h"

namespace art {

enum TimeUnit {
  kTimeUnitNanosecond,
  kTimeUnitMicrosecond,
  kTimeUnitMillisecond,
  kTimeUnitSecond,
};

// Constants for common time periods.
constexpr unsigned int kOneMinuteInSeconds = 60;
constexpr unsigned int kOneHourInSeconds = 60 * kOneMinuteInSeconds;

// Returns a human-readable time string which prints every nanosecond while trying to limit the
// number of trailing zeros. Prints using the largest human readable unit up to a second.
// e.g. "1ms", "1.000000001s", "1.001us"
std::string PrettyDuration(uint64_t nano_duration, size_t max_fraction_digits = 3);

// Format a nanosecond time to specified units.
std::string FormatDuration(uint64_t nano_duration, TimeUnit time_unit,
                           size_t max_fraction_digits);

// Get the appropriate unit for a nanosecond duration.
TimeUnit GetAppropriateTimeUnit(uint64_t nano_duration);

// Get the divisor to convert from a nanoseconds to a time unit.
uint64_t GetNsToTimeUnitDivisor(TimeUnit time_unit);

// Returns the current date in ISO yyyy-mm-dd hh:mm:ss format.
std::string GetIsoDate();

// Returns the monotonic time since some unspecified starting point in milliseconds.
uint64_t MilliTime();

// Returns the monotonic time since some unspecified starting point in microseconds.
uint64_t MicroTime();

// Returns the monotonic time since some unspecified starting point in nanoseconds.
uint64_t NanoTime();

// Returns the thread-specific CPU-time clock in nanoseconds or -1 if unavailable.
uint64_t ThreadCpuNanoTime();

// Returns the process CPU-time clock in nanoseconds or -1 if unavailable.
uint64_t ProcessCpuNanoTime();

// Converts the given number of nanoseconds to milliseconds.
static constexpr uint64_t NsToMs(uint64_t ns) {
  return ns / 1000 / 1000;
}

// Converts the given number of nanoseconds to microseconds.
static constexpr uint64_t NsToUs(uint64_t ns) {
  return ns / 1000;
}

// Converts the given number of milliseconds to nanoseconds
static constexpr uint64_t MsToNs(uint64_t ms) {
  return ms * 1000 * 1000;
}

// Converts the given number of milliseconds to microseconds
static constexpr uint64_t MsToUs(uint64_t ms) {
  return ms * 1000;
}

static constexpr uint64_t UsToNs(uint64_t us) {
  return us * 1000;
}

static constexpr uint64_t SecondsToMs(uint64_t seconds) {
  return seconds * 1000;
}

static constexpr time_t SaturatedTimeT(int64_t secs) {
  if (sizeof(time_t) < sizeof(int64_t)) {
    return static_cast<time_t>(std::min(secs,
                                        static_cast<int64_t>(std::numeric_limits<time_t>::max())));
  } else {
    return secs;
  }
}

#if defined(__APPLE__)
#ifndef CLOCK_REALTIME
// No clocks to specify on OS/X < 10.12, fake value to pass to routines that require a clock.
#define CLOCK_REALTIME 0xebadf00d
#endif
#endif

// Sleep for the given number of nanoseconds, a bad way to handle contention.
void NanoSleep(uint64_t ns);

// Initialize a timespec to either a relative time (ms,ns), or to the absolute
// time corresponding to the indicated clock value plus the supplied offset.
void InitTimeSpec(bool absolute, int clock, int64_t ms, int32_t ns, timespec* ts);

// Converts `timespec` to nanoseconds. The return value can be negative, which should be interpreted
// as a time before the epoch.
static constexpr int64_t TimeSpecToNs(timespec ts) {
  DCHECK_GE(ts.tv_nsec, 0);  // According to POSIX.
  return static_cast<int64_t>(ts.tv_sec) * INT64_C(1000000000) + ts.tv_nsec;
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_TIME_UTILS_H_
