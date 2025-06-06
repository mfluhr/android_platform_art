/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "metrics.h"

#include "base/macros.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "metrics_test.h"

#pragma clang diagnostic push
#pragma clang diagnostic error "-Wconversion"

namespace art {
namespace metrics {

using test::CounterValue;
using test::GetBuckets;
using test::TestBackendBase;

class MetricsTest : public ::testing::Test {};

TEST_F(MetricsTest, SimpleCounter) {
  MetricsCounter<DatumId::kClassVerificationTotalTime> test_counter;

  EXPECT_EQ(0u, CounterValue(test_counter));

  test_counter.AddOne();
  EXPECT_EQ(1u, CounterValue(test_counter));

  test_counter.Add(5);
  EXPECT_EQ(6u, CounterValue(test_counter));
}

TEST_F(MetricsTest, CounterTimer) {
  MetricsCounter<DatumId::kClassVerificationTotalTime> test_counter;
  {
    AutoTimer timer{&test_counter};
    // Sleep for 2µs so the counter will be greater than 0.
    NanoSleep(2'000);
  }
  EXPECT_GT(CounterValue(test_counter), 0u);
}

TEST_F(MetricsTest, CounterTimerExplicitStop) {
  MetricsCounter<DatumId::kClassVerificationTotalTime> test_counter;
  AutoTimer timer{&test_counter};
  // Sleep for 2µs so the counter will be greater than 0.
  NanoSleep(2'000);
  timer.Stop();
  EXPECT_GT(CounterValue(test_counter), 0u);
}

TEST_F(MetricsTest, CounterTimerExplicitStart) {
  MetricsCounter<DatumId::kClassVerificationTotalTime> test_counter;
  {
    AutoTimer timer{&test_counter, /*autostart=*/false};
    // Sleep for 2µs so the counter will be greater than 0.
    NanoSleep(2'000);
  }
  EXPECT_EQ(CounterValue(test_counter), 0u);

  {
    AutoTimer timer{&test_counter, /*autostart=*/false};
    timer.Start();
    // Sleep for 2µs so the counter will be greater than 0.
    NanoSleep(2'000);
  }
  EXPECT_GT(CounterValue(test_counter), 0u);
}

TEST_F(MetricsTest, CounterTimerExplicitStartStop) {
  MetricsCounter<DatumId::kClassVerificationTotalTime> test_counter;
  AutoTimer timer{&test_counter, /*autostart=*/false};
  // Sleep for 2µs so the counter will be greater than 0.
  timer.Start();
  NanoSleep(2'000);
  timer.Stop();
  EXPECT_GT(CounterValue(test_counter), 0u);
}

TEST_F(MetricsTest, AccumulatorMetric) {
  MetricsAccumulator<DatumId::kClassLoadingTotalTime, uint64_t, std::max> accumulator;

  std::vector<std::thread> threads;

  constexpr uint64_t kMaxValue = 100;

  for (uint64_t i = 0; i <= kMaxValue; i++) {
    threads.emplace_back(std::thread{[&accumulator, i]() { accumulator.Add(i); }});
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(CounterValue(accumulator), kMaxValue);
}

TEST_F(MetricsTest, AverageMetric) {
  MetricsAverage<DatumId::kClassLoadingTotalTime, uint64_t> avg;

  std::vector<std::thread> threads;

  constexpr uint64_t kMaxValue = 100;

  for (uint64_t i = 0; i <= kMaxValue; i++) {
    threads.emplace_back(std::thread{[&avg, i]() { avg.Add(i); }});
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(CounterValue(avg), (kMaxValue + 1) / 2);
}

TEST_F(MetricsTest, DatumName) {
  EXPECT_EQ("ClassVerificationTotalTime", DatumName(DatumId::kClassVerificationTotalTime));
}

TEST_F(MetricsTest, SimpleHistogramTest) {
  MetricsHistogram<DatumId::kYoungGcCollectionTime, 5, 0, 100> histogram;

  // bucket 0: 0-19
  histogram.Add(10);

  // bucket 1: 20-39
  histogram.Add(20);
  histogram.Add(25);

  // bucket 2: 40-59
  histogram.Add(56);
  histogram.Add(57);
  histogram.Add(58);
  histogram.Add(59);

  // bucket 3: 60-79
  histogram.Add(70);
  histogram.Add(70);
  histogram.Add(70);

  // bucket 4: 80-99
  // leave this bucket empty

  std::vector<uint32_t> buckets{GetBuckets(histogram)};
  EXPECT_EQ(1u, buckets[0u]);
  EXPECT_EQ(2u, buckets[1u]);
  EXPECT_EQ(4u, buckets[2u]);
  EXPECT_EQ(3u, buckets[3u]);
  EXPECT_EQ(0u, buckets[4u]);
}

// Make sure values added outside the range of the histogram go into the first or last bucket.
TEST_F(MetricsTest, HistogramOutOfRangeTest) {
  MetricsHistogram<DatumId::kYoungGcCollectionTime, 2, 0, 100> histogram;

  // bucket 0: 0-49
  histogram.Add(-500);

  // bucket 1: 50-99
  histogram.Add(250);
  histogram.Add(1000);

  std::vector<uint32_t> buckets{GetBuckets(histogram)};
  EXPECT_EQ(1u, buckets[0u]);
  EXPECT_EQ(2u, buckets[1u]);
}

// Test adding values to ArtMetrics and reporting them through a test backend.
TEST_F(MetricsTest, ArtMetricsReport) {
  ArtMetrics metrics;

  // Collect some data
  static constexpr uint64_t verification_time = 42;
  metrics.ClassVerificationTotalTime()->Add(verification_time);
  // Add a negative value so we are guaranteed that it lands in the first bucket.
  metrics.YoungGcCollectionTime()->Add(-5);

  // Report and check the data
  class TestBackend : public TestBackendBase {
   public:
    ~TestBackend() {
      EXPECT_TRUE(found_counter_);
      EXPECT_TRUE(found_histogram_);
    }

    void ReportCounter(DatumId counter_type, uint64_t value) override {
      switch (counter_type) {
        case DatumId::kClassVerificationTotalTime:
          EXPECT_EQ(value, verification_time)
              << "Unexpected value for counter " << DatumName(counter_type);
          found_counter_ = true;
          break;
        case DatumId::kTimeElapsedDelta:
          // TimeElapsedData can be greater than 0 if the test takes more than 1ms to run
          EXPECT_GE(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
          break;
        default:
          EXPECT_EQ(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
      }
    }

    void ReportHistogram(DatumId histogram_type,
                         int64_t,
                         int64_t,
                         const std::vector<uint32_t>& buckets) override {
      if (histogram_type == DatumId::kYoungGcCollectionTime) {
        EXPECT_EQ(buckets[0], 1u) << "Unexpected value for bucket 0 for histogram "
                                  << DatumName(histogram_type);
        for (size_t i = 1; i < buckets.size(); ++i) {
          EXPECT_EQ(buckets[i], 0u) << "Unexpected value for bucket " << i << " for histogram "
                                    << DatumName(histogram_type);
        }
        found_histogram_ = true;
      } else {
        for (size_t i = 0; i < buckets.size(); ++i) {
          EXPECT_EQ(buckets[i], 0u) << "Unexpected value for bucket " << i << " for histogram "
                                    << DatumName(histogram_type);
        }
      }
    }

   private:
    bool found_counter_{false};
    bool found_histogram_{false};
  } backend;

  metrics.ReportAllMetricsAndResetValueMetrics({&backend});
}

TEST_F(MetricsTest, HistogramTimer) {
  MetricsHistogram<DatumId::kYoungGcCollectionTime, 1, 0, 100> test_histogram;
  {
    AutoTimer timer{&test_histogram};
    // Sleep for 2µs so the counter will be greater than 0.
    NanoSleep(2'000);
  }

  EXPECT_GT(GetBuckets(test_histogram)[0], 0u);
}

// Makes sure all defined metrics are included when dumping through StreamBackend.
TEST_F(MetricsTest, StreamBackendDumpAllMetrics) {
  ArtMetrics metrics;
  StringBackend backend(std::make_unique<TextFormatter>());

  metrics.ReportAllMetricsAndResetValueMetrics({&backend});

  // Make sure the resulting string lists all the metrics.
  const std::string result = backend.GetAndResetBuffer();
#define METRIC(name, type, ...) \
  EXPECT_NE(result.find(DatumName(DatumId::k##name)), std::string::npos);
  ART_METRICS(METRIC);
#undef METRIC
}

TEST_F(MetricsTest, ResetMetrics) {
  ArtMetrics metrics;

  // Add something to each of the metrics.
#define METRIC(name, type, ...) metrics.name()->Add(42);
  ART_METRICS(METRIC)
#undef METRIC

  class NonZeroBackend : public TestBackendBase {
   public:
    void ReportCounter(DatumId counter_type, uint64_t value) override {
      EXPECT_NE(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
    }

    void ReportHistogram(DatumId histogram_type,
                         [[maybe_unused]] int64_t minimum_value,
                         [[maybe_unused]] int64_t maximum_value,
                         const std::vector<uint32_t>& buckets) override {
      bool nonzero = false;
      for (const auto value : buckets) {
        nonzero |= (value != 0u);
      }
      EXPECT_TRUE(nonzero) << "Unexpected value for histogram " << DatumName(histogram_type);
    }
  } non_zero_backend;

  // Make sure the metrics all have a nonzero value.
  metrics.ReportAllMetricsAndResetValueMetrics({&non_zero_backend});

  // Reset the metrics and make sure they are all zero again
  metrics.Reset();

  class ZeroBackend : public TestBackendBase {
   public:
    void ReportCounter(DatumId counter_type, uint64_t value) override {
      if (counter_type == DatumId::kTimeElapsedDelta) {
        // TimeElapsedData can be greater than 0 if the test takes more than 1ms to run
        EXPECT_GE(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
      } else {
        EXPECT_EQ(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
      }
    }

    void ReportHistogram([[maybe_unused]] DatumId histogram_type,
                         [[maybe_unused]] int64_t minimum_value,
                         [[maybe_unused]] int64_t maximum_value,
                         const std::vector<uint32_t>& buckets) override {
      for (const auto value : buckets) {
        EXPECT_EQ(value, 0u) << "Unexpected value for histogram " << DatumName(histogram_type);
      }
    }
  } zero_backend;

  metrics.ReportAllMetricsAndResetValueMetrics({&zero_backend});
}

TEST_F(MetricsTest, KeepEventMetricsResetValueMetricsAfterReporting) {
  ArtMetrics metrics;

  // Add something to each of the metrics.
#define METRIC(name, type, ...) metrics.name()->Add(42);
  ART_METRICS(METRIC)
#undef METRIC

  class FirstBackend : public TestBackendBase {
   public:
    void ReportCounter(DatumId counter_type, uint64_t value) override {
      EXPECT_NE(value, 0u) << "Unexpected value for counter " << DatumName(counter_type);
    }

    void ReportHistogram(DatumId histogram_type,
                         [[maybe_unused]] int64_t minimum_value,
                         [[maybe_unused]] int64_t maximum_value,
                         const std::vector<uint32_t>& buckets) override {
      EXPECT_NE(buckets[0], 0u) << "Unexpected value for bucket 0 for histogram "
                                << DatumName(histogram_type);
      for (size_t i = 1; i < buckets.size(); i++) {
        EXPECT_EQ(buckets[i], 0u) << "Unexpected value for bucket " << i << " for histogram "
                                  << DatumName(histogram_type);
      }
    }
  } first_backend;

  // Make sure the metrics all have a nonzero value, and they are not reset between backends.
  metrics.ReportAllMetricsAndResetValueMetrics({&first_backend, &first_backend});

  // After reporting, the Value Metrics should have been reset.
  class SecondBackend : public TestBackendBase {
   public:
    void ReportCounter(DatumId datum_id, uint64_t value) override {
      switch (datum_id) {
        // Value metrics - expected to have been reset
#define CHECK_METRIC(name, ...) case DatumId::k##name:
        ART_VALUE_METRICS(CHECK_METRIC)
#undef CHECK_METRIC
        if (datum_id == DatumId::kTimeElapsedDelta) {
          // TimeElapsedData can be greater than 0 if the test takes more than 1ms to run
          EXPECT_GE(value, 0u) << "Unexpected value for counter " << DatumName(datum_id);
        } else {
          EXPECT_EQ(value, 0u) << "Unexpected value for counter " << DatumName(datum_id);
        }
        return;

        // Event metrics - expected to have retained their previous value
#define CHECK_METRIC(name, ...) case DatumId::k##name:
        ART_EVENT_METRICS(CHECK_METRIC)
#undef CHECK_METRIC
        EXPECT_NE(value, 0u) << "Unexpected value for metric " << DatumName(datum_id);
        return;

        default:
          // unknown metric - it should not be possible to reach this path
          FAIL();
          UNREACHABLE();
      }
    }

    // All histograms are event metrics.
    void ReportHistogram([[maybe_unused]] DatumId histogram_type,
                         [[maybe_unused]] int64_t minimum_value,
                         [[maybe_unused]] int64_t maximum_value,
                         const std::vector<uint32_t>& buckets) override {
      EXPECT_NE(buckets[0], 0u) << "Unexpected value for bucket 0 for histogram "
                                << DatumName(histogram_type);
      for (size_t i = 1; i < buckets.size(); i++) {
        EXPECT_EQ(buckets[i], 0u) << "Unexpected value for bucket " << i << " for histogram "
                                  << DatumName(histogram_type);
      }
    }
  } second_backend;

  metrics.ReportAllMetricsAndResetValueMetrics({&second_backend});
}

TEST(TextFormatterTest, ReportMetrics_WithBuckets) {
  TextFormatter text_formatter;
  SessionData session_data{
      .session_id = 1000,
      .uid = 50,
      .compilation_reason = CompilationReason::kInstall,
      .compiler_filter = CompilerFilterReporting::kSpeed,
  };

  text_formatter.FormatBeginReport(200, session_data);
  text_formatter.FormatReportCounter(DatumId::kFullGcCount, 1u);
  text_formatter.FormatReportHistogram(DatumId::kFullGcCollectionTime, 50, 200, {2, 4, 7, 1});
  text_formatter.FormatEndReport();

  const std::string result = text_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "\n*** ART internal metrics ***\n"
            "  Metadata:\n"
            "    timestamp_since_start_ms: 200\n"
            "    session_id: 1000\n"
            "    uid: 50\n"
            "    compilation_reason: install\n"
            "    compiler_filter: speed\n"
            "  Metrics:\n"
            "    FullGcCount: count = 1\n"
            "    FullGcCollectionTime: range = 50...200, buckets: 2,4,7,1\n"
            "*** Done dumping ART internal metrics ***\n");
}

TEST(TextFormatterTest, ReportMetrics_NoBuckets) {
  TextFormatter text_formatter;
  SessionData session_data{
      .session_id = 500,
      .uid = 15,
      .compilation_reason = CompilationReason::kCmdLine,
      .compiler_filter = CompilerFilterReporting::kExtract,
  };

  text_formatter.FormatBeginReport(400, session_data);
  text_formatter.FormatReportHistogram(DatumId::kFullGcCollectionTime, 10, 20, {});
  text_formatter.FormatEndReport();

  std::string result = text_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "\n*** ART internal metrics ***\n"
            "  Metadata:\n"
            "    timestamp_since_start_ms: 400\n"
            "    session_id: 500\n"
            "    uid: 15\n"
            "    compilation_reason: cmdline\n"
            "    compiler_filter: extract\n"
            "  Metrics:\n"
            "    FullGcCollectionTime: range = 10...20, no buckets\n"
            "*** Done dumping ART internal metrics ***\n");
}

TEST(TextFormatterTest, BeginReport_NoSessionData) {
  TextFormatter text_formatter;
  std::optional<SessionData> empty_session_data;

  text_formatter.FormatBeginReport(100, empty_session_data);
  text_formatter.FormatEndReport();

  std::string result = text_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "\n*** ART internal metrics ***\n"
            "  Metadata:\n"
            "    timestamp_since_start_ms: 100\n"
            "  Metrics:\n"
            "*** Done dumping ART internal metrics ***\n");
}

TEST(TextFormatterTest, GetAndResetBuffer_ActuallyResetsBuffer) {
  TextFormatter text_formatter;
  std::optional<SessionData> empty_session_data;

  text_formatter.FormatBeginReport(200, empty_session_data);
  text_formatter.FormatReportCounter(DatumId::kFullGcCount, 1u);
  text_formatter.FormatEndReport();

  std::string result = text_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "\n*** ART internal metrics ***\n"
            "  Metadata:\n"
            "    timestamp_since_start_ms: 200\n"
            "  Metrics:\n"
            "    FullGcCount: count = 1\n"
            "*** Done dumping ART internal metrics ***\n");

  text_formatter.FormatBeginReport(300, empty_session_data);
  text_formatter.FormatReportCounter(DatumId::kFullGcCount, 5u);
  text_formatter.FormatEndReport();

  result = text_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "\n*** ART internal metrics ***\n"
            "  Metadata:\n"
            "    timestamp_since_start_ms: 300\n"
            "  Metrics:\n"
            "    FullGcCount: count = 5\n"
            "*** Done dumping ART internal metrics ***\n");
}

TEST(XmlFormatterTest, ReportMetrics_WithBuckets) {
  XmlFormatter xml_formatter;
  SessionData session_data{
      .session_id = 123,
      .uid = 456,
      .compilation_reason = CompilationReason::kFirstBoot,
      .compiler_filter = CompilerFilterReporting::kSpace,
  };

  xml_formatter.FormatBeginReport(250, session_data);
  xml_formatter.FormatReportCounter(DatumId::kYoungGcCount, 3u);
  xml_formatter.FormatReportHistogram(DatumId::kYoungGcCollectionTime, 300, 600, {1, 5, 3});
  xml_formatter.FormatEndReport();

  const std::string result = xml_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "<art_runtime_metrics>"
            "<version>1.0</version>"
            "<metadata>"
            "<timestamp_since_start_ms>250</timestamp_since_start_ms>"
            "<session_id>123</session_id>"
            "<uid>456</uid>"
            "<compilation_reason>first-boot</compilation_reason>"
            "<compiler_filter>space</compiler_filter>"
            "</metadata>"
            "<metrics>"
            "<YoungGcCount>"
            "<counter_type>count</counter_type>"
            "<value>3</value>"
            "</YoungGcCount>"
            "<YoungGcCollectionTime>"
            "<counter_type>histogram</counter_type>"
            "<minimum_value>300</minimum_value>"
            "<maximum_value>600</maximum_value>"
            "<buckets>"
            "<bucket>1</bucket>"
            "<bucket>5</bucket>"
            "<bucket>3</bucket>"
            "</buckets>"
            "</YoungGcCollectionTime>"
            "</metrics>"
            "</art_runtime_metrics>");
}

TEST(XmlFormatterTest, ReportMetrics_NoBuckets) {
  XmlFormatter xml_formatter;
  SessionData session_data{
      .session_id = 234,
      .uid = 345,
      .compilation_reason = CompilationReason::kFirstBoot,
      .compiler_filter = CompilerFilterReporting::kSpace,
  };

  xml_formatter.FormatBeginReport(160, session_data);
  xml_formatter.FormatReportCounter(DatumId::kYoungGcCount, 4u);
  xml_formatter.FormatReportHistogram(DatumId::kYoungGcCollectionTime, 20, 40, {});
  xml_formatter.FormatEndReport();

  const std::string result = xml_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "<art_runtime_metrics>"
            "<version>1.0</version>"
            "<metadata>"
            "<timestamp_since_start_ms>160</timestamp_since_start_ms>"
            "<session_id>234</session_id>"
            "<uid>345</uid>"
            "<compilation_reason>first-boot</compilation_reason>"
            "<compiler_filter>space</compiler_filter>"
            "</metadata>"
            "<metrics>"
            "<YoungGcCount>"
            "<counter_type>count</counter_type>"
            "<value>4</value>"
            "</YoungGcCount>"
            "<YoungGcCollectionTime>"
            "<counter_type>histogram</counter_type>"
            "<minimum_value>20</minimum_value>"
            "<maximum_value>40</maximum_value>"
            "<buckets/>"
            "</YoungGcCollectionTime>"
            "</metrics>"
            "</art_runtime_metrics>");
}

TEST(XmlFormatterTest, BeginReport_NoSessionData) {
  XmlFormatter xml_formatter;
  std::optional<SessionData> empty_session_data;

  xml_formatter.FormatBeginReport(100, empty_session_data);
  xml_formatter.FormatReportCounter(DatumId::kYoungGcCount, 3u);
  xml_formatter.FormatEndReport();

  std::string result = xml_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "<art_runtime_metrics>"
            "<version>1.0</version>"
            "<metadata>"
            "<timestamp_since_start_ms>100</timestamp_since_start_ms>"
            "</metadata>"
            "<metrics>"
            "<YoungGcCount>"
            "<counter_type>count</counter_type>"
            "<value>3</value>"
            "</YoungGcCount>"
            "</metrics>"
            "</art_runtime_metrics>");
}

TEST(XmlFormatterTest, GetAndResetBuffer_ActuallyResetsBuffer) {
  XmlFormatter xml_formatter;
  std::optional<SessionData> empty_session_data;

  xml_formatter.FormatBeginReport(200, empty_session_data);
  xml_formatter.FormatReportCounter(DatumId::kFullGcCount, 1u);
  xml_formatter.FormatEndReport();

  std::string result = xml_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "<art_runtime_metrics>"
            "<version>1.0</version>"
            "<metadata>"
            "<timestamp_since_start_ms>200</timestamp_since_start_ms>"
            "</metadata>"
            "<metrics>"
            "<FullGcCount>"
            "<counter_type>count</counter_type>"
            "<value>1</value>"
            "</FullGcCount>"
            "</metrics>"
            "</art_runtime_metrics>");

  xml_formatter.FormatBeginReport(300, empty_session_data);
  xml_formatter.FormatReportCounter(DatumId::kFullGcCount, 5u);
  xml_formatter.FormatEndReport();

  result = xml_formatter.GetAndResetBuffer();
  ASSERT_EQ(result,
            "<art_runtime_metrics>"
            "<version>1.0</version>"
            "<metadata>"
            "<timestamp_since_start_ms>300</timestamp_since_start_ms>"
            "</metadata>"
            "<metrics>"
            "<FullGcCount>"
            "<counter_type>count</counter_type>"
            "<value>5</value>"
            "</FullGcCount>"
            "</metrics>"
            "</art_runtime_metrics>");
}

TEST(CompilerFilterReportingTest, FromName) {
  ASSERT_EQ(CompilerFilterReportingFromName("error"), CompilerFilterReporting::kError);
  ASSERT_EQ(CompilerFilterReportingFromName("unknown"), CompilerFilterReporting::kUnknown);
  ASSERT_EQ(CompilerFilterReportingFromName("assume-verified"),
            CompilerFilterReporting::kAssumeVerified);
  ASSERT_EQ(CompilerFilterReportingFromName("extract"), CompilerFilterReporting::kExtract);
  ASSERT_EQ(CompilerFilterReportingFromName("verify"), CompilerFilterReporting::kVerify);
  ASSERT_EQ(CompilerFilterReportingFromName("space-profile"),
            CompilerFilterReporting::kSpaceProfile);
  ASSERT_EQ(CompilerFilterReportingFromName("space"), CompilerFilterReporting::kSpace);
  ASSERT_EQ(CompilerFilterReportingFromName("speed-profile"),
            CompilerFilterReporting::kSpeedProfile);
  ASSERT_EQ(CompilerFilterReportingFromName("speed"), CompilerFilterReporting::kSpeed);
  ASSERT_EQ(CompilerFilterReportingFromName("everything-profile"),
            CompilerFilterReporting::kEverythingProfile);
  ASSERT_EQ(CompilerFilterReportingFromName("everything"), CompilerFilterReporting::kEverything);
  ASSERT_EQ(CompilerFilterReportingFromName("run-from-apk"), CompilerFilterReporting::kRunFromApk);
  ASSERT_EQ(CompilerFilterReportingFromName("run-from-apk-fallback"),
            CompilerFilterReporting::kRunFromApkFallback);
}

TEST(CompilerFilterReportingTest, Name) {
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kError), "error");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kUnknown), "unknown");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kAssumeVerified),
            "assume-verified");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kExtract), "extract");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kVerify), "verify");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kSpaceProfile), "space-profile");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kSpace), "space");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kSpeedProfile), "speed-profile");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kSpeed), "speed");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kEverythingProfile),
            "everything-profile");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kEverything), "everything");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kRunFromApk), "run-from-apk");
  ASSERT_EQ(CompilerFilterReportingName(CompilerFilterReporting::kRunFromApkFallback),
            "run-from-apk-fallback");
}

TEST(CompilerReason, FromName) {
  ASSERT_EQ(CompilationReasonFromName("unknown"), CompilationReason::kUnknown);
  ASSERT_EQ(CompilationReasonFromName("first-boot"), CompilationReason::kFirstBoot);
  ASSERT_EQ(CompilationReasonFromName("boot-after-ota"), CompilationReason::kBootAfterOTA);
  ASSERT_EQ(CompilationReasonFromName("post-boot"), CompilationReason::kPostBoot);
  ASSERT_EQ(CompilationReasonFromName("install"), CompilationReason::kInstall);
  ASSERT_EQ(CompilationReasonFromName("install-fast"), CompilationReason::kInstallFast);
  ASSERT_EQ(CompilationReasonFromName("install-bulk"), CompilationReason::kInstallBulk);
  ASSERT_EQ(CompilationReasonFromName("install-bulk-secondary"),
            CompilationReason::kInstallBulkSecondary);
  ASSERT_EQ(CompilationReasonFromName("install-bulk-downgraded"),
            CompilationReason::kInstallBulkDowngraded);
  ASSERT_EQ(CompilationReasonFromName("install-bulk-secondary-downgraded"),
            CompilationReason::kInstallBulkSecondaryDowngraded);
  ASSERT_EQ(CompilationReasonFromName("bg-dexopt"), CompilationReason::kBgDexopt);
  ASSERT_EQ(CompilationReasonFromName("ab-ota"), CompilationReason::kABOTA);
  ASSERT_EQ(CompilationReasonFromName("inactive"), CompilationReason::kInactive);
  ASSERT_EQ(CompilationReasonFromName("shared"), CompilationReason::kShared);
  ASSERT_EQ(CompilationReasonFromName("install-with-dex-metadata"),
            CompilationReason::kInstallWithDexMetadata);
  ASSERT_EQ(CompilationReasonFromName("prebuilt"), CompilationReason::kPrebuilt);
  ASSERT_EQ(CompilationReasonFromName("cmdline"), CompilationReason::kCmdLine);
  ASSERT_EQ(CompilationReasonFromName("error"), CompilationReason::kError);
  ASSERT_EQ(CompilationReasonFromName("vdex"), CompilationReason::kVdex);
  ASSERT_EQ(CompilationReasonFromName("boot-after-mainline-update"),
            CompilationReason::kBootAfterMainlineUpdate);
}

TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterMainlineUpdate),
            "boot-after-mainline-update");
}
}  // namespace metrics
}  // namespace art

#pragma clang diagnostic pop  // -Wconversion
