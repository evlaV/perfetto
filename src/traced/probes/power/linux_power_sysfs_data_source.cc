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

#include "src/traced/probes/power/linux_power_sysfs_data_source.h"

#include <dirent.h>
#include <sys/types.h>
#include <cstring>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/power/linux_power_sysfs_config.pbzero.h"
#include "protos/perfetto/trace/power/battery_counters.pbzero.h"
#include "protos/perfetto/trace/power/power_rails.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kMinPollIntervalMs = 100;
constexpr uint32_t kDefaultPollIntervalMs = 1000;

// Onboard power monitor ICs (e.g. Steam Frame's) are exposed as hwmon
// devices whose driver name starts with this prefix.
constexpr char kPowerMonitorHwmonPrefix[] = "max34417";
constexpr size_t kPowerMonitorMaxChannels = 4;

std::optional<int64_t> ReadFileAsInt64(std::string path) {
  std::string buf;
  if (!base::ReadFile(path, &buf))
    return std::nullopt;
  return base::StringToInt64(base::StripSuffix(buf, "\n"));
}
}  // namespace

LinuxPowerSysfsDataSource::BatteryInfo::BatteryInfo(
    const char* power_supply_dir_path)
    : power_supply_dir_path_(power_supply_dir_path) {
  base::ScopedDir power_supply_dir(opendir(power_supply_dir_path_.c_str()));
  if (!power_supply_dir)
    return;

  for (auto* ent = readdir(power_supply_dir.get()); ent;
       ent = readdir(power_supply_dir.get())) {
    if (ent->d_name[0] == '.')
      continue;
    std::string dir_name =
        std::string(power_supply_dir_path) + "/" + ent->d_name;
    std::string buf;
    if (!base::ReadFile(dir_name + "/type", &buf) ||
        base::StripSuffix(buf, "\n") != "Battery")
      continue;
    buf.clear();
    if (!base::ReadFile(dir_name + "/present", &buf) ||
        base::StripSuffix(buf, "\n") != "1")
      continue;
    sysfs_battery_subdirs_.push_back(ent->d_name);
  }
}
LinuxPowerSysfsDataSource::BatteryInfo::~BatteryInfo() = default;

size_t LinuxPowerSysfsDataSource::BatteryInfo::num_batteries() const {
  return sysfs_battery_subdirs_.size();
}

std::optional<int64_t>
LinuxPowerSysfsDataSource::BatteryInfo::GetChargeCounterUah(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/charge_now");
}

std::optional<int64_t>
LinuxPowerSysfsDataSource::BatteryInfo::GetEnergyCounterUah(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/energy_now");
}

std::optional<int64_t> LinuxPowerSysfsDataSource::BatteryInfo::GetVoltageUv(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/voltage_now");
}

std::optional<int64_t>
LinuxPowerSysfsDataSource::BatteryInfo::GetCapacityPercent(size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/capacity");
}

std::optional<int64_t> LinuxPowerSysfsDataSource::BatteryInfo::GetCurrentNowUa(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/current_now");
}

std::optional<int64_t>
LinuxPowerSysfsDataSource::BatteryInfo::GetAverageCurrentUa(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return ReadFileAsInt64(power_supply_dir_path_ + "/" +
                         sysfs_battery_subdirs_[battery_idx] + "/current_avg");
}

std::string LinuxPowerSysfsDataSource::BatteryInfo::GetBatteryName(
    size_t battery_idx) {
  PERFETTO_CHECK(battery_idx < sysfs_battery_subdirs_.size());
  return sysfs_battery_subdirs_[battery_idx];
}

LinuxPowerSysfsDataSource::PowerRailInfo::PowerRailInfo(
    const char* hwmon_dir_path) {
  base::ScopedDir hwmon_dir(opendir(hwmon_dir_path));
  if (!hwmon_dir)
    return;

  for (auto* ent = readdir(hwmon_dir.get()); ent;
       ent = readdir(hwmon_dir.get())) {
    if (ent->d_name[0] == '.')
      continue;
    std::string dir_path = std::string(hwmon_dir_path) + "/" + ent->d_name;

    std::string driver_name;
    if (!base::ReadFile(dir_path + "/name", &driver_name))
      continue;
    driver_name = base::StripSuffix(driver_name, "\n");
    if (driver_name.compare(0, strlen(kPowerMonitorHwmonPrefix),
                            kPowerMonitorHwmonPrefix) != 0) {
      continue;
    }

    for (size_t ch = 1; ch <= kPowerMonitorMaxChannels; ch++) {
      std::string ch_str = std::to_string(ch);
      std::string input_path = dir_path + "/power" + ch_str + "_input";
      if (!base::FileExists(input_path))
        continue;
      std::string label;
      if (base::ReadFile(dir_path + "/power" + ch_str + "_label", &label)) {
        label = base::StripSuffix(label, "\n");
      } else {
        label = driver_name + "_power" + ch_str;
      }
      rails_.push_back(Rail{input_path, label});
    }
  }
}

LinuxPowerSysfsDataSource::PowerRailInfo::~PowerRailInfo() = default;

size_t LinuxPowerSysfsDataSource::PowerRailInfo::num_rails() const {
  return rails_.size();
}

const std::string& LinuxPowerSysfsDataSource::PowerRailInfo::GetRailName(
    size_t rail_idx) const {
  PERFETTO_CHECK(rail_idx < rails_.size());
  return rails_[rail_idx].name;
}

std::optional<int64_t> LinuxPowerSysfsDataSource::PowerRailInfo::GetPowerUw(
    size_t rail_idx) const {
  PERFETTO_CHECK(rail_idx < rails_.size());
  return ReadFileAsInt64(rails_[rail_idx].power_input_path);
}

LinuxPowerSysfsDataSource::ChargerDaemonInfo::ChargerDaemonInfo(
    const char* state_dir_path)
    : state_dir_path_(state_dir_path) {}

std::optional<double>
LinuxPowerSysfsDataSource::ChargerDaemonInfo::GetCapacityPercent() const {
  std::string buf;
  if (!base::ReadFile(state_dir_path_ + "/battery_percent", &buf))
    return std::nullopt;
  return base::StringToDouble(base::StripSuffix(buf, "\n"));
}

// static
const ProbesDataSource::Descriptor LinuxPowerSysfsDataSource::descriptor = {
    /*name*/ "linux.sysfs_power",
    /*flags*/ Descriptor::kFlagsNone,
    /*fill_descriptor_func*/ nullptr,
};

LinuxPowerSysfsDataSource::LinuxPowerSysfsDataSource(
    DataSourceConfig cfg,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  protos::pbzero::LinuxPowerSysfsConfig::Decoder pcfg(
      cfg.linux_power_sysfs_config_raw());
  poll_interval_ms_ = pcfg.poll_ms();
  if (poll_interval_ms_ == 0)
    poll_interval_ms_ = kDefaultPollIntervalMs;
  if (poll_interval_ms_ < kMinPollIntervalMs) {
    PERFETTO_ELOG("linux.sysfs_power poll interval of %" PRIu32
                  " ms is too low. Capping to %" PRIu32 " ms",
                  poll_interval_ms_, kMinPollIntervalMs);
    poll_interval_ms_ = kMinPollIntervalMs;
  }

  // Generate a session UUID for the power rail energy counters, using
  // boottime and thread ID (see AndroidPowerDataSource for the rationale).
  uint64_t boottime_bits =
      static_cast<uint64_t>(base::GetBootTimeNs().count()) & 0x7FFFFFFFFULL;
  uint64_t thread_bits = static_cast<uint64_t>(base::GetThreadId()) << 35;
  power_rails_session_uuid_ = boottime_bits | thread_bits;
}

LinuxPowerSysfsDataSource::~LinuxPowerSysfsDataSource() = default;

void LinuxPowerSysfsDataSource::Start() {
  battery_info_.reset(new BatteryInfo());
  power_rail_info_.reset(new PowerRailInfo());
  power_rail_energy_uws_.assign(power_rail_info_->num_rails(), 0.0);
  charger_daemon_info_.reset(new ChargerDaemonInfo());
  Tick();
}

void LinuxPowerSysfsDataSource::Tick() {
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, LINUX_POWER_SYSFS_TICK);

  // Post next task.
  auto now_ms = base::GetWallTimeMs().count();
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->Tick();
      },
      poll_interval_ms_ - static_cast<uint32_t>(now_ms % poll_interval_ms_));

  WriteBatteryCounters();
  WritePowerRailCounters();
}

void LinuxPowerSysfsDataSource::WriteBatteryCounters() {
  // Query battery counters from sysfs. Report the battery counters for each
  // battery.
  for (size_t battery_idx = 0; battery_idx < battery_info_->num_batteries();
       battery_idx++) {
    auto packet = writer_->NewTracePacket();
    packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));

    auto* counters_proto = packet->set_battery();
    auto value = battery_info_->GetChargeCounterUah(battery_idx);
    if (value)
      counters_proto->set_charge_counter_uah(*value);
    // Prefer the charging daemon's reported capacity for the main battery:
    // it accounts for edge cases (e.g. USB-supplemented discharge) that the
    // raw fuel-gauge sysfs counter above doesn't.
    std::optional<double> charger_capacity_percent;
    if (battery_idx == 0 && charger_daemon_info_)
      charger_capacity_percent = charger_daemon_info_->GetCapacityPercent();
    if (charger_capacity_percent) {
      counters_proto->set_capacity_percent(
          static_cast<float>(*charger_capacity_percent));
    } else {
      value = battery_info_->GetCapacityPercent(battery_idx);
      if (value)
        counters_proto->set_capacity_percent(static_cast<float>(*value));
    }
    value = battery_info_->GetCurrentNowUa(battery_idx);
    if (value)
      counters_proto->set_current_ua(*value);
    value = battery_info_->GetAverageCurrentUa(battery_idx);
    if (value)
      counters_proto->set_current_ua(*value);
    value = battery_info_->GetEnergyCounterUah(battery_idx);
    if (value)
      counters_proto->set_energy_counter_uwh(*value);
    value = battery_info_->GetVoltageUv(battery_idx);
    if (value)
      counters_proto->set_voltage_uv(*value);
    // On systems with multiple batteries, disambiguate with battery names.
    if (battery_info_->num_batteries() > 1)
      counters_proto->set_name(battery_info_->GetBatteryName(battery_idx));
  }

  // The charging daemon can report a capacity even when the kernel doesn't
  // expose a standard sysfs battery for it. Don't drop that reading.
  if (battery_info_->num_batteries() == 0 && charger_daemon_info_) {
    auto charger_capacity_percent = charger_daemon_info_->GetCapacityPercent();
    if (charger_capacity_percent) {
      auto packet = writer_->NewTracePacket();
      packet->set_timestamp(
          static_cast<uint64_t>(base::GetBootTimeNs().count()));
      packet->set_battery()->set_capacity_percent(
          static_cast<float>(*charger_capacity_percent));
    }
  }
}

void LinuxPowerSysfsDataSource::WritePowerRailCounters() {
  if (power_rail_info_->num_rails() == 0)
    return;

  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));

  auto* rails_proto = packet->set_power_rails();
  rails_proto->set_session_uuid(power_rails_session_uuid_);

  if (should_emit_power_rail_descriptors_) {
    for (size_t i = 0; i < power_rail_info_->num_rails(); i++) {
      auto* rail_desc_proto = rails_proto->add_rail_descriptor();
      rail_desc_proto->set_index(static_cast<uint32_t>(i));
      rail_desc_proto->set_rail_name(power_rail_info_->GetRailName(i));
    }
    should_emit_power_rail_descriptors_ = false;
  }

  // The rails report instantaneous power, but PowerRails.EnergyData carries
  // an accumulated energy counter, so integrate power over the poll period
  // (a simple Riemann sum) into a running per-rail energy counter.
  const double poll_interval_s = poll_interval_ms_ / 1000.0;
  const uint64_t timestamp_ms =
      static_cast<uint64_t>(base::GetBootTimeNs().count() / 1000000);
  for (size_t i = 0; i < power_rail_info_->num_rails(); i++) {
    auto power_uw = power_rail_info_->GetPowerUw(i);
    if (!power_uw)
      continue;
    power_rail_energy_uws_[i] +=
        static_cast<double>(*power_uw) * poll_interval_s;

    auto* energy_data_proto = rails_proto->add_energy_data();
    energy_data_proto->set_index(static_cast<uint32_t>(i));
    energy_data_proto->set_timestamp_ms(timestamp_ms);
    energy_data_proto->set_energy(
        static_cast<uint64_t>(power_rail_energy_uws_[i]));
  }
}

void LinuxPowerSysfsDataSource::Flush(FlushRequestID,
                                      std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
