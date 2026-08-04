#pragma once

// Tiny debug HTTP for pulling session recordings + logs from a Host Agent.
// Listens on media_port+1. Auth: Authorization: Bearer <psk> or ?psk=

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace road_desk::agent {

class DebugHttpServer {
 public:
  DebugHttpServer() = default;
  ~DebugHttpServer() { stop(); }

  // Non-blocking. Serves <exe>/debug and host-agent.log.
  bool start(int listen_port, const std::string& psk);
  void stop();
  int port() const { return port_; }

 private:
  void run();

  std::thread thr_;
  std::atomic<bool> stop_{false};
  std::uintptr_t listen_ = 0;  // SOCKET
  int port_ = 0;
  std::string psk_;
};

}  // namespace road_desk::agent
