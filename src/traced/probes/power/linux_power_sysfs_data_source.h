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

#ifndef SRC_TRACED_PROBES_POWER_LINUX_POWER_SYSFS_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_POWER_LINUX_POWER_SYSFS_DATA_SOURCE_H_

#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {
class BatteryInfo;
class TraceWriter;

namespace base {
class TaskRunner;
}

class LinuxPowerSysfsDataSource : public ProbesDataSource {
 public:
  class BatteryInfo {
   public:
    explicit BatteryInfo(
        const char* power_supply_dir_path = "/sys/class/power_supply");
    ~BatteryInfo();

    // The current coloumb counter value in µAh.
    std::optional<int64_t> GetChargeCounterUah(size_t battery_idx);

    // The current energy counter in µWh.
    std::optional<int64_t> GetEnergyCounterUah(size_t battery_idx);

    // The voltage in µV.
    std::optional<int64_t> GetVoltageUv(size_t battery_idx);

    // The battery capacity in percent.
    std::optional<int64_t> GetCapacityPercent(size_t battery_idx);

    // The current reading of the battery in µA.
    std::optional<int64_t> GetCurrentNowUa(size_t battery_idx);

    // The smoothed current reading of the battery in µA.
    std::optional<int64_t> GetAverageCurrentUa(size_t battery_idx);

    // Name of the battery.
    std::string GetBatteryName(size_t battery_idx);

    size_t num_batteries() const;

   private:
    std::string power_supply_dir_path_;
    // The subdirectories that contain info of a battery power supply, e.g.
    // BAT0.
    std::vector<std::string> sysfs_battery_subdirs_;
  };

  // Reads the instantaneous power draw of each rail reported by an onboard
  // power monitor IC that's exposed through the Linux hwmon subsystem (e.g.
  // the max34417-based monitor on Steam Frame).
  class PowerRailInfo {
   public:
    explicit PowerRailInfo(const char* hwmon_dir_path = "/sys/class/hwmon");
    ~PowerRailInfo();

    size_t num_rails() const;
    const std::string& GetRailName(size_t rail_idx) const;

    // The instantaneous power draw of the rail, in microwatts.
    std::optional<int64_t> GetPowerUw(size_t rail_idx) const;

   private:
    struct Rail {
      std::string power_input_path;  // e.g. .../hwmon3/power2_input
      std::string name;
    };
    std::vector<Rail> rails_;
  };

  // Reads the battery state as last reported by the Steam Frame charging
  // daemon (deckard-charger), which applies edge-case handling (e.g.
  // USB-supplemented discharge) that the raw fuel-gauge sysfs counters used
  // by BatteryInfo don't capture. Absent on non-Steam-Frame devices.
  class ChargerDaemonInfo {
   public:
    explicit ChargerDaemonInfo(
        const char* state_dir_path = "/run/deckardcharger");

    // The battery capacity, in percent, as last reported by the charger.
    std::optional<double> GetCapacityPercent() const;

   private:
    std::string state_dir_path_;
  };

  static const ProbesDataSource::Descriptor descriptor;

  LinuxPowerSysfsDataSource(DataSourceConfig,
                            base::TaskRunner*,
                            TracingSessionID,
                            std::unique_ptr<TraceWriter> writer);

  ~LinuxPowerSysfsDataSource() override;

  base::WeakPtr<LinuxPowerSysfsDataSource> GetWeakPtr() const;

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;
  // Use the default ClearIncrementalState() implementation: this data source
  // doesn't have any incremental state.

 private:
  void Tick();
  void WriteBatteryCounters();
  void WritePowerRailCounters();

  uint32_t poll_interval_ms_ = 0;

  // Accumulated energy per rail, in microwatt-seconds, integrated from the
  // instantaneous power readings. Indexed like PowerRailInfo's rails.
  std::vector<double> power_rail_energy_uws_;
  bool should_emit_power_rail_descriptors_ = true;
  uint64_t power_rails_session_uuid_ = 0;

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;
  std::unique_ptr<BatteryInfo> battery_info_;
  std::unique_ptr<PowerRailInfo> power_rail_info_;
  std::unique_ptr<ChargerDaemonInfo> charger_daemon_info_;
  base::WeakPtrFactory<LinuxPowerSysfsDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_POWER_LINUX_POWER_SYSFS_DATA_SOURCE_H_
