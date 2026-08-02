#pragma once

#include <string>

namespace road_desk::agent {

struct GatewayConfig {
  std::string agent_id;
  std::string gateway_url;  // e.g. http://192.168.1.10:8743
  std::string agent_psk;
};

// %ProgramData%\RoadDesk\agent.json
std::string gateway_config_path();

bool load_gateway_config(GatewayConfig* out);
bool save_gateway_config(const GatewayConfig& cfg);

// Modal dialog when no config file. Returns false if user cancels.
bool prompt_gateway_config(GatewayConfig* out);

}  // namespace road_desk::agent
