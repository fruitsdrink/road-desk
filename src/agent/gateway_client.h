#pragma once

#include "gateway_config.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace road_desk::agent {

class GatewayClient {
 public:
  GatewayClient() = default;
  ~GatewayClient();

  GatewayClient(const GatewayClient&) = delete;
  GatewayClient& operator=(const GatewayClient&) = delete;

  // Non-blocking: starts background register/heartbeat. Never exits process on failure.
  void start(const GatewayConfig& cfg, int media_port, const std::string& version);
  void stop();

 private:
  void run();
  bool post_json(const char* path, const std::string& body);
  std::vector<std::string> list_ipv4s() const;
  std::string preferred_ipv4() const;
  std::string build_body() const;

  GatewayConfig cfg_;
  int media_port_ = 38471;
  std::string version_;
  std::atomic<bool> stop_{false};
  std::thread thr_;
};

}  // namespace road_desk::agent
