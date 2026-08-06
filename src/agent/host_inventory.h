#pragma once

#include <string>

namespace road_desk::agent {

// Machine inventory JSON for gateway heartbeat (UTF-8 object, no outer key).
// Static fields: os, osVersion, arch, computerName, cpu, memoryTotalMb, disks[].
// Live sample (each heartbeat): cpuUsagePercent, memoryLoadPercent, memoryUsedMb,
// memoryAvailMb, uptimeSec, cpuCount.
std::string collect_host_inventory_json();

}  // namespace road_desk::agent
