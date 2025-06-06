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

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#include "garbage_collector.h"

#include "android-base/stringprintf.h"

#include "base/dumpable.h"
#include "base/histogram-inl.h"
#include "base/logging.h"  // For VLOG_IS_ON.
#include "base/mutex-inl.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/gc_pause_listener.h"
#include "gc/heap.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "runtime.h"
#include "thread-current-inl.h"
#include "thread_list.h"

namespace art HIDDEN {
namespace gc {
namespace collector {

namespace {

// Report a GC metric via the ATrace interface.
void TraceGCMetric(const char* name, int64_t value) {
  // ART's interface with systrace (through libartpalette) only supports
  // reporting 32-bit (signed) integer values at the moment. Upon
  // underflows/overflows, clamp metric values at `int32_t` min/max limits and
  // report these events via a corresponding underflow/overflow counter; also
  // log a warning about the first underflow/overflow occurrence.
  //
  // TODO(b/300015145): Consider extending libarpalette to allow reporting this
  // value as a 64-bit (signed) integer (instead of a 32-bit (signed) integer).
  // Note that this is likely unnecessary at the moment (November 2023) for any
  // size-related GC metric, given the maximum theoretical size of a managed
  // heap (4 GiB).
  if (UNLIKELY(value < std::numeric_limits<int32_t>::min())) {
    ATraceIntegerValue(name, std::numeric_limits<int32_t>::min());
    std::string underflow_counter_name = std::string(name) + " int32_t underflow";
    ATraceIntegerValue(underflow_counter_name.c_str(), 1);
    static bool int32_underflow_reported = false;
    if (!int32_underflow_reported) {
      LOG(WARNING) << "GC Metric \"" << name << "\" with value " << value
                   << " causing a 32-bit integer underflow";
      int32_underflow_reported = true;
    }
    return;
  }
  if (UNLIKELY(value > std::numeric_limits<int32_t>::max())) {
    ATraceIntegerValue(name, std::numeric_limits<int32_t>::max());
    std::string overflow_counter_name = std::string(name) + " int32_t overflow";
    ATraceIntegerValue(overflow_counter_name.c_str(), 1);
    static bool int32_overflow_reported = false;
    if (!int32_overflow_reported) {
      LOG(WARNING) << "GC Metric \"" << name << "\" with value " << value
                   << " causing a 32-bit integer overflow";
      int32_overflow_reported = true;
    }
    return;
  }
  ATraceIntegerValue(name, value);
}

}  // namespace

Iteration::Iteration()
    : duration_ns_(0), timings_("GC iteration timing logger", true, VLOG_IS_ON(heap)) {
  Reset(kGcCauseBackground, false);  // Reset to some place holder values.
}

void Iteration::Reset(GcCause gc_cause, bool clear_soft_references) {
  timings_.Reset();
  pause_times_.clear();
  duration_ns_ = 0;
  app_slow_path_duration_ms_ = 0;
  bytes_scanned_ = 0;
  clear_soft_references_ = clear_soft_references;
  gc_cause_ = gc_cause;
  freed_ = ObjectBytePair();
  freed_los_ = ObjectBytePair();
  freed_bytes_revoke_ = 0;
}

uint64_t Iteration::GetEstimatedThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (static_cast<uint64_t>(freed_.bytes) * 1000) / (NsToMs(GetDurationNs()) + 1);
}

GarbageCollector::GarbageCollector(Heap* heap, const std::string& name)
    : heap_(heap),
      name_(name),
      pause_histogram_((name_ + " paused").c_str(), kPauseBucketSize, kPauseBucketCount),
      rss_histogram_((name_ + " peak-rss").c_str(), kMemBucketSize, kMemBucketCount),
      freed_bytes_histogram_((name_ + " freed-bytes").c_str(), kMemBucketSize, kMemBucketCount),
      gc_time_histogram_(nullptr),
      metrics_gc_count_(nullptr),
      metrics_gc_count_delta_(nullptr),
      gc_throughput_histogram_(nullptr),
      gc_tracing_throughput_hist_(nullptr),
      gc_throughput_avg_(nullptr),
      gc_tracing_throughput_avg_(nullptr),
      gc_scanned_bytes_(nullptr),
      gc_scanned_bytes_delta_(nullptr),
      gc_freed_bytes_(nullptr),
      gc_freed_bytes_delta_(nullptr),
      gc_duration_(nullptr),
      gc_duration_delta_(nullptr),
      cumulative_timings_(name),
      pause_histogram_lock_("pause histogram lock", kDefaultMutexLevel, true),
      is_transaction_active_(false),
      are_metrics_initialized_(false) {
  ResetMeasurements();
}

void GarbageCollector::RegisterPause(uint64_t nano_length) {
  GetCurrentIteration()->pause_times_.push_back(nano_length);
}

uint64_t GarbageCollector::ExtractRssFromMincore(
    std::list<std::pair<void*, void*>>* gc_ranges) {
  uint64_t rss = 0;
  if (gc_ranges->empty()) {
    return 0;
  }
  // mincore() is linux-specific syscall.
#if defined(__linux__)
  using range_t = std::pair<void*, void*>;
  // Sort gc_ranges
  gc_ranges->sort([](const range_t& a, const range_t& b) {
    return std::less()(a.first, b.first);
  });
  // Merge gc_ranges. It's necessary because the kernel may merge contiguous
  // regions if their properties match. This is sufficient as kernel doesn't
  // merge those adjoining ranges which differ only in name.
  size_t vec_len = 0;
  for (auto it = gc_ranges->begin(); it != gc_ranges->end(); it++) {
    auto next_it = it;
    next_it++;
    while (next_it != gc_ranges->end()) {
      if (it->second == next_it->first) {
        it->second = next_it->second;
        next_it = gc_ranges->erase(next_it);
      } else {
        break;
      }
    }
    size_t length = static_cast<uint8_t*>(it->second) - static_cast<uint8_t*>(it->first);
    // Compute max length for vector allocation later.
    vec_len = std::max(vec_len, DivideByPageSize(length));
  }
  std::unique_ptr<unsigned char[]> vec(new unsigned char[vec_len]);
  for (const auto it : *gc_ranges) {
    size_t length = static_cast<uint8_t*>(it.second) - static_cast<uint8_t*>(it.first);
    if (mincore(it.first, length, vec.get()) == 0) {
      for (size_t i = 0; i < DivideByPageSize(length); i++) {
        // Least significant bit represents residency of a page. Other bits are
        // reserved.
        rss += vec[i] & 0x1;
      }
    } else {
      LOG(WARNING) << "Call to mincore() on memory range [0x" << std::hex << it.first
                   << ", 0x" << it.second << std::dec << ") failed: " << strerror(errno);
    }
  }
  rss *= gPageSize;
  rss_histogram_.AddValue(rss / KB);
#endif
  return rss;
}

void GarbageCollector::Run(GcCause gc_cause, bool clear_soft_references) {
  ScopedTrace trace(android::base::StringPrintf("%s %s GC", PrettyCause(gc_cause), GetName()));
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  uint64_t start_time = NanoTime();
  uint64_t thread_cpu_start_time = ThreadCpuNanoTime();
  GetHeap()->CalculatePreGcWeightedAllocatedBytes();
  Iteration* current_iteration = GetCurrentIteration();
  current_iteration->Reset(gc_cause, clear_soft_references);
  // Note transaction mode is single-threaded and there's no asynchronous GC and this flag doesn't
  // change in the middle of a GC.
  is_transaction_active_ = runtime->IsActiveTransaction();
  RunPhases();  // Run all the GC phases.
  GetHeap()->CalculatePostGcWeightedAllocatedBytes();
  // Add the current timings to the cumulative timings.
  cumulative_timings_.AddLogger(*GetTimings());
  // Update cumulative statistics with how many bytes the GC iteration freed.
  total_freed_objects_ += current_iteration->GetFreedObjects() +
      current_iteration->GetFreedLargeObjects();
  total_scanned_bytes_ += current_iteration->GetScannedBytes();
  int64_t freed_bytes = current_iteration->GetFreedBytes() +
      current_iteration->GetFreedLargeObjectBytes();
  total_freed_bytes_ += freed_bytes;
  // Rounding negative freed bytes to 0 as we are not interested in such corner cases.
  freed_bytes_histogram_.AddValue(std::max<int64_t>(freed_bytes / KB, 0));
  uint64_t end_time = NanoTime();
  uint64_t thread_cpu_end_time = ThreadCpuNanoTime();
  total_thread_cpu_time_ns_ += thread_cpu_end_time - thread_cpu_start_time;
  uint64_t duration_ns = end_time - start_time;
  current_iteration->SetDurationNs(duration_ns);
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The entire GC was paused, clear the fake pauses which might be in the pause times and add
    // the whole GC duration.
    current_iteration->pause_times_.clear();
    RegisterPause(duration_ns);
  }
  total_time_ns_ += duration_ns;
  uint64_t total_pause_time_ns = 0;
  for (uint64_t pause_time : current_iteration->GetPauseTimes()) {
    MutexLock mu(self, pause_histogram_lock_);
    pause_histogram_.AdjustAndAddValue(pause_time);
    total_pause_time_ns += pause_time;
  }
  metrics::ArtMetrics* metrics = runtime->GetMetrics();
  // Report STW pause time in microseconds.
  const uint64_t total_pause_time_us = total_pause_time_ns / 1'000;
  metrics->WorldStopTimeDuringGCAvg()->Add(total_pause_time_us);
  metrics->GcWorldStopTime()->Add(total_pause_time_us);
  metrics->GcWorldStopTimeDelta()->Add(total_pause_time_us);
  metrics->GcWorldStopCount()->AddOne();
  metrics->GcWorldStopCountDelta()->AddOne();
  // Report total collection time of all GCs put together.
  metrics->TotalGcCollectionTime()->Add(NsToMs(duration_ns));
  metrics->TotalGcCollectionTimeDelta()->Add(NsToMs(duration_ns));
  if (are_metrics_initialized_) {
    metrics_gc_count_->Add(1);
    metrics_gc_count_delta_->Add(1);
    // Report GC time in milliseconds.
    gc_time_histogram_->Add(NsToMs(duration_ns));
    // Throughput in bytes/s. Add 1us to prevent possible division by 0.
    uint64_t throughput = (current_iteration->GetScannedBytes() * 1'000'000)
            / (NsToUs(duration_ns) + 1);
    // Report in MB/s.
    throughput /= MB;
    gc_tracing_throughput_hist_->Add(throughput);
    gc_tracing_throughput_avg_->Add(throughput);

    // Report GC throughput in MB/s.
    throughput = current_iteration->GetEstimatedThroughput() / MB;
    gc_throughput_histogram_->Add(throughput);
    gc_throughput_avg_->Add(throughput);

    gc_scanned_bytes_->Add(current_iteration->GetScannedBytes());
    gc_scanned_bytes_delta_->Add(current_iteration->GetScannedBytes());
    gc_freed_bytes_->Add(current_iteration->GetFreedBytes());
    gc_freed_bytes_delta_->Add(current_iteration->GetFreedBytes());
    gc_duration_->Add(NsToMs(current_iteration->GetDurationNs()));
    gc_duration_delta_->Add(NsToMs(current_iteration->GetDurationNs()));
    gc_app_slow_path_during_gc_duration_delta_->Add(current_iteration->GetAppSlowPathDurationMs());
  }

  // Report some metrics via the ATrace interface, to surface them in Perfetto.
  TraceGCMetric("freed_normal_object_bytes", current_iteration->GetFreedBytes());
  TraceGCMetric("freed_large_object_bytes", current_iteration->GetFreedLargeObjectBytes());
  TraceGCMetric("freed_bytes", freed_bytes);

  is_transaction_active_ = false;
}

void GarbageCollector::SwapBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
  // these bitmaps. The bitmap swapping is an optimization so that we do not need to clear the live
  // bits of dead objects in the live bitmap.
  const GcType gc_type = GetGcType();
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    // We never allocate into zygote spaces.
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect ||
        (gc_type == kGcTypeFull &&
         space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
      if (space->GetLiveBitmap() != nullptr && !space->HasBoundBitmaps()) {
        CHECK(space->IsContinuousMemMapAllocSpace());
        space->AsContinuousMemMapAllocSpace()->SwapBitmaps();
      }
    }
  }
  for (const auto& disc_space : GetHeap()->GetDiscontinuousSpaces()) {
    disc_space->AsLargeObjectSpace()->SwapBitmaps();
  }
}

void GarbageCollector::SweepArray(accounting::ObjectStack* allocations,
                                  bool swap_bitmaps,
                                  std::vector<space::ContinuousSpace*>* sweep_spaces) {
  Thread* self = Thread::Current();
  static constexpr size_t kSweepArrayChunkFreeSize = 1024;
  mirror::Object** chunk_free_buffer = new mirror::Object*[kSweepArrayChunkFreeSize];
  size_t chunk_free_pos = 0;
  ObjectBytePair freed;
  ObjectBytePair freed_los;
  // How many objects are left in the array, modified after each space is swept.
  StackReference<mirror::Object>* objects = allocations->Begin();
  size_t count = allocations->Size();
  // Start by sweeping the continuous spaces.
  for (space::ContinuousSpace* space : *sweep_spaces) {
    space::AllocSpace* alloc_space = space->AsAllocSpace();
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (swap_bitmaps) {
      std::swap(live_bitmap, mark_bitmap);
    }
    StackReference<mirror::Object>* out = objects;
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* const obj = objects[i].AsMirrorPtr();
      if (obj == nullptr) {
        continue;
      }
      if (space->HasAddress(obj)) {
        // This object is in the space, remove it from the array and add it to the sweep buffer
        // if needed.
        if (!mark_bitmap->Test(obj)) {
          // Handle the case where buffer allocation failed.
          if (LIKELY(chunk_free_buffer != nullptr)) {
            if (chunk_free_pos >= kSweepArrayChunkFreeSize) {
              TimingLogger::ScopedTiming t2("FreeList", GetTimings());
              freed.objects += chunk_free_pos;
              freed.bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
              chunk_free_pos = 0;
            }
            chunk_free_buffer[chunk_free_pos++] = obj;
          } else {
            freed.objects++;
            freed.bytes += alloc_space->Free(self, obj);
          }
        }
      } else {
        (out++)->Assign(obj);
      }
    }
    if (chunk_free_pos > 0) {
      TimingLogger::ScopedTiming t2("FreeList", GetTimings());
      freed.objects += chunk_free_pos;
      freed.bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
      chunk_free_pos = 0;
    }
    // All of the references which space contained are no longer in the allocation stack, update
    // the count.
    count = out - objects;
  }
  if (chunk_free_buffer != nullptr) {
    delete[] chunk_free_buffer;
  }
  // Handle the large object space.
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  if (large_object_space != nullptr) {
    accounting::LargeObjectBitmap* large_live_objects = large_object_space->GetLiveBitmap();
    accounting::LargeObjectBitmap* large_mark_objects = large_object_space->GetMarkBitmap();
    if (swap_bitmaps) {
      std::swap(large_live_objects, large_mark_objects);
    }
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* const obj = objects[i].AsMirrorPtr();
      // Handle large objects.
      if (kUseThreadLocalAllocationStack && obj == nullptr) {
        continue;
      }
      if (!large_mark_objects->Test(obj)) {
        ++freed_los.objects;
        freed_los.bytes += large_object_space->Free(self, obj);
      }
    }
  }
  {
    TimingLogger::ScopedTiming t2("RecordFree", GetTimings());
    RecordFree(freed);
    RecordFreeLOS(freed_los);
    t2.NewTiming("ResetStack");
    allocations->Reset();
  }
}

uint64_t GarbageCollector::GetEstimatedMeanThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (total_freed_bytes_ * 1000) / (NsToMs(GetCumulativeTimings().GetTotalNs()) + 1);
}

void GarbageCollector::ResetMeasurements() {
  {
    MutexLock mu(Thread::Current(), pause_histogram_lock_);
    pause_histogram_.Reset();
  }
  cumulative_timings_.Reset();
  rss_histogram_.Reset();
  freed_bytes_histogram_.Reset();
  total_thread_cpu_time_ns_ = 0u;
  total_time_ns_ = 0u;
  total_freed_objects_ = 0u;
  total_freed_bytes_ = 0;
  total_scanned_bytes_ = 0u;
}

GarbageCollector::ScopedPause::ScopedPause(GarbageCollector* collector, bool with_reporting)
    : start_time_(NanoTime()), collector_(collector), with_reporting_(with_reporting) {
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll(__FUNCTION__);
  if (with_reporting) {
    GcPauseListener* pause_listener = runtime->GetHeap()->GetGcPauseListener();
    if (pause_listener != nullptr) {
      pause_listener->StartPause();
    }
  }
}

GarbageCollector::ScopedPause::~ScopedPause() {
  collector_->RegisterPause(NanoTime() - start_time_);
  Runtime* runtime = Runtime::Current();
  if (with_reporting_) {
    GcPauseListener* pause_listener = runtime->GetHeap()->GetGcPauseListener();
    if (pause_listener != nullptr) {
      pause_listener->EndPause();
    }
  }
  runtime->GetThreadList()->ResumeAll();
}

// Returns the current GC iteration and assocated info.
Iteration* GarbageCollector::GetCurrentIteration() {
  return heap_->GetCurrentGcIteration();
}
const Iteration* GarbageCollector::GetCurrentIteration() const {
  return heap_->GetCurrentGcIteration();
}

bool GarbageCollector::ShouldEagerlyReleaseMemoryToOS() const {
  // We have seen old kernels and custom kernel features misbehave in the
  // presence of too much usage of MADV_FREE. So only release memory eagerly
  // on platforms we know do not have the bug.
  static const bool gEnableLazyRelease = !kIsTargetBuild || IsKernelVersionAtLeast(6, 0);
  if (!gEnableLazyRelease) {
    return true;
  }
  Runtime* runtime = Runtime::Current();
  // Zygote isn't a memory heavy process, we should always instantly release memory to the OS.
  if (runtime->IsZygote()) {
    return true;
  }
  if (GetCurrentIteration()->GetGcCause() == kGcCauseExplicit &&
      !runtime->IsEagerlyReleaseExplicitGcDisabled()) {
    // Our behavior with explicit GCs is to always release any available memory.
    return true;
  }
  // Keep on the memory if the app is in foreground. If it is in background or
  // goes into the background (see invocation with cause kGcCauseCollectorTransition),
  // release the memory.
  return !runtime->InJankPerceptibleProcessState();
}

void GarbageCollector::RecordFree(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}
void GarbageCollector::RecordFreeLOS(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_los_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}

uint64_t GarbageCollector::GetTotalPausedTimeNs() {
  MutexLock mu(Thread::Current(), pause_histogram_lock_);
  return pause_histogram_.AdjustedSum();
}

void GarbageCollector::DumpPerformanceInfo(std::ostream& os) {
  const CumulativeLogger& logger = GetCumulativeTimings();
  const size_t iterations = logger.GetIterations();
  if (iterations == 0) {
    return;
  }
  os << Dumpable<CumulativeLogger>(logger);
  const uint64_t total_ns = logger.GetTotalNs();
  const double seconds = NsToMs(total_ns) / 1000.0;
  const uint64_t freed_bytes = GetTotalFreedBytes();
  const uint64_t scanned_bytes = GetTotalScannedBytes();
  {
    MutexLock mu(Thread::Current(), pause_histogram_lock_);
    if (pause_histogram_.SampleSize() > 0) {
      Histogram<uint64_t>::CumulativeData cumulative_data;
      pause_histogram_.CreateHistogram(&cumulative_data);
      pause_histogram_.PrintConfidenceIntervals(os, 0.99, cumulative_data);
    }
  }
#if defined(__linux__)
  if (rss_histogram_.SampleSize() > 0) {
    os << rss_histogram_.Name()
       << ": Avg: " << PrettySize(rss_histogram_.Mean() * KB)
       << " Max: " << PrettySize(rss_histogram_.Max() * KB)
       << " Min: " << PrettySize(rss_histogram_.Min() * KB) << "\n";
    os << "Peak-rss Histogram: ";
    rss_histogram_.DumpBins(os);
    os << "\n";
  }
#endif
  if (freed_bytes_histogram_.SampleSize() > 0) {
    os << freed_bytes_histogram_.Name()
       << ": Avg: " << PrettySize(freed_bytes_histogram_.Mean() * KB)
       << " Max: " << PrettySize(freed_bytes_histogram_.Max() * KB)
       << " Min: " << PrettySize(freed_bytes_histogram_.Min() * KB) << "\n";
    os << "Freed-bytes histogram: ";
    freed_bytes_histogram_.DumpBins(os);
    os << "\n";
  }
  const double cpu_seconds = NsToMs(GetTotalCpuTime()) / 1000.0;
  os << GetName() << " total time: " << PrettyDuration(total_ns)
     << " mean time: " << PrettyDuration(total_ns / iterations) << "\n"
     << GetName() << " freed: " << PrettySize(freed_bytes) << "\n"
     << GetName() << " throughput: " << PrettySize(freed_bytes / seconds) << "/s"
     << "  per cpu-time: "
     << static_cast<uint64_t>(freed_bytes / cpu_seconds) << "/s / "
     << PrettySize(freed_bytes / cpu_seconds) << "/s\n"
     << GetName() << " tracing throughput: "
     << PrettySize(scanned_bytes / seconds) << "/s "
     << " per cpu-time: "
     << PrettySize(scanned_bytes / cpu_seconds) << "/s\n";
}

}  // namespace collector
}  // namespace gc
}  // namespace art
