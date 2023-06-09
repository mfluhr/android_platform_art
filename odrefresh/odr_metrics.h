/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef ART_ODREFRESH_ODR_METRICS_H_
#define ART_ODREFRESH_ODR_METRICS_H_

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

#include "base/macros.h"
#include "exec_utils.h"
#include "odr_metrics_record.h"

namespace art {
namespace odrefresh {

class OdrMetrics final {
 public:
  // Enumeration used to track the latest stage reached running odrefresh.
  //
  // These values mirror those in OdrefreshReported::Stage in
  // frameworks/proto_logging/atoms/art/odrefresh_extension_atoms.proto.
  // NB There are gaps between the values in case an additional stages are introduced.
  enum class Stage : uint8_t {
    kUnknown = 0,
    kCheck = 10,
    kPreparation = 20,
    kPrimaryBootClasspath = 30,
    kSecondaryBootClasspath = 40,
    kSystemServerClasspath = 50,
    kComplete = 60,
  };

  // Enumeration describing the overall status, processing stops on the first error discovered.
  //
  // These values mirror those in OdrefreshReported::Status in
  // frameworks/proto_logging/atoms/art/odrefresh_extension_atoms.proto.
  enum class Status : uint8_t {
    kUnknown = 0,
    kOK = 1,
    kNoSpace = 2,
    kIoError = 3,
    kDex2OatError = 4,
    // Value 5 was kTimeLimitExceeded, but has been removed in favour of
    // reporting the exit code for Dex2Oat (set to ExecResult::kTimedOut)
    kStagingFailed = 6,
    kInstallFailed = 7,
    // Failed to access the dalvik-cache directory due to lack of permission.
    kDalvikCachePermissionDenied = 8,
  };

  // Enumeration describing the cause of compilation (if any) in odrefresh.
  //
  // These values mirror those in OdrefreshReported::Trigger in
  // frameworks/proto_logging/atoms/art/odrefresh_extension_atoms.proto.
  enum class Trigger : uint8_t {
    kUnknown = 0,
    kApexVersionMismatch = 1,
    kDexFilesChanged = 2,
    kMissingArtifacts = 3,
  };

  // Enumeration describing the type of boot classpath compilation in odrefresh.
  //
  // These values mirror those in OdrefreshReported::BcpCompilationType in
  // frameworks/proto_logging/atoms/art/odrefresh_extension_atoms.proto.
  enum class BcpCompilationType : uint8_t {
    kUnknown = 0,
    // Compiles for both the primary boot image and the mainline extension.
    kPrimaryAndMainline = 1,
    // Only compiles for the mainline extension.
    kMainline = 2,
  };

  explicit OdrMetrics(const std::string& cache_directory,
                      const std::string& metrics_file = kOdrefreshMetricsFile);
  ~OdrMetrics();

  // Enables/disables metrics writing.
  void SetEnabled(bool value) { enabled_ = value; }

  // Gets the ART APEX that metrics are being collected on behalf of.
  int64_t GetArtApexVersion() const { return art_apex_version_; }

  // Sets the ART APEX that metrics are being collected on behalf of.
  void SetArtApexVersion(int64_t version) { art_apex_version_ = version; }

  // Gets the ART APEX last update time in milliseconds.
  int64_t GetArtApexLastUpdateMillis() const { return art_apex_last_update_millis_; }

  // Sets the ART APEX last update time in milliseconds.
  void SetArtApexLastUpdateMillis(int64_t last_update_millis) {
    art_apex_last_update_millis_ = last_update_millis;
  }

  // Gets the trigger for metrics collection. The trigger is the reason why odrefresh considers
  // compilation necessary.
  Trigger GetTrigger() const { return trigger_; }

  // Sets the trigger for metrics collection. The trigger is the reason why odrefresh considers
  // compilation necessary. Only call this method if compilation is necessary as the presence
  // of a trigger means we will try to record and upload metrics.
  void SetTrigger(const Trigger trigger) { trigger_ = trigger; }

  // Sets the execution status of the current odrefresh processing stage.
  void SetStatus(const Status status) { status_ = status; }

  // Sets the current odrefresh processing stage.
  void SetStage(Stage stage) { stage_ = stage; }

  // Sets the result of the current dex2oat invocation.
  void SetDex2OatResult(Stage stage,
                        int64_t compilation_time,
                        const std::optional<ExecResult>& dex2oat_result);

  // Sets the BCP compilation type.
  void SetBcpCompilationType(Stage stage, BcpCompilationType type);

  // Captures the current free space as the end free space.
  void CaptureSpaceFreeEnd();

  // Records metrics into an OdrMetricsRecord.
  OdrMetricsRecord ToRecord() const;

 private:
  OdrMetrics(const OdrMetrics&) = delete;
  OdrMetrics operator=(const OdrMetrics&) = delete;

  static int32_t GetFreeSpaceMiB(const std::string& path);
  static void WriteToFile(const std::string& path, const OdrMetrics* metrics);

  static OdrMetricsRecord::Dex2OatExecResult
  ConvertExecResult(const std::optional<ExecResult>& result);

  const std::string cache_directory_;
  const std::string metrics_file_;

  bool enabled_ = false;

  int64_t art_apex_version_ = 0;
  int64_t art_apex_last_update_millis_ = 0;
  Trigger trigger_ = Trigger::kUnknown;
  Stage stage_ = Stage::kUnknown;
  Status status_ = Status::kUnknown;

  int32_t cache_space_free_start_mib_ = 0;
  int32_t cache_space_free_end_mib_ = 0;

  // The total time spent on compiling primary BCP.
  int32_t primary_bcp_compilation_millis_ = 0;

  // The result of the dex2oat invocation for compiling primary BCP, or `std::nullopt` if dex2oat is
  // not invoked.
  std::optional<ExecResult> primary_bcp_dex2oat_result_;

  BcpCompilationType primary_bcp_compilation_type_ = BcpCompilationType::kUnknown;

  // The total time spent on compiling secondary BCP.
  int32_t secondary_bcp_compilation_millis_ = 0;

  // The result of the dex2oat invocation for compiling secondary BCP, or `std::nullopt` if dex2oat
  // is not invoked.
  std::optional<ExecResult> secondary_bcp_dex2oat_result_;

  BcpCompilationType secondary_bcp_compilation_type_ = BcpCompilationType::kUnknown;

  // The total time spent on compiling system server.
  int32_t system_server_compilation_millis_ = 0;

  // The result of the last dex2oat invocation for compiling system server, or `std::nullopt` if
  // dex2oat is not invoked.
  std::optional<ExecResult> system_server_dex2oat_result_;
};

// Generated ostream operators.
std::ostream& operator<<(std::ostream& os, OdrMetrics::Status status);
std::ostream& operator<<(std::ostream& os, OdrMetrics::Stage stage);
std::ostream& operator<<(std::ostream& os, OdrMetrics::Trigger trigger);

}  // namespace odrefresh
}  // namespace art

#endif  // ART_ODREFRESH_ODR_METRICS_H_
