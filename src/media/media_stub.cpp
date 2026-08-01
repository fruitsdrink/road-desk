// Stub media plane when ROAD_DESK_WITH_LIBVNC is OFF.

#include "media_plane.h"

#include <string>

namespace road_desk::media {

struct MediaPlane::Impl {
  bool running = false;
  int port = 0;
};

MediaPlane::MediaPlane() : impl_(new Impl) {}
MediaPlane::~MediaPlane() {
  delete impl_;
  impl_ = nullptr;
}

bool MediaPlane::listen(const MediaPlaneConfig& config) {
  impl_->running = true;
  impl_->port = config.listen_port;
  return true;
}

void MediaPlane::serve() {
  impl_->running = false;
}

void MediaPlane::request_stop() {
  if (impl_) {
    impl_->running = false;
  }
}

bool MediaPlane::running() const {
  return impl_ && impl_->running;
}

int MediaPlane::bound_port() const {
  return impl_ ? impl_->port : 0;
}

std::string MediaPlane::tls_fingerprint_sha256() const {
  return {};
}

struct MediaClient::Impl {};

MediaClient::MediaClient() : impl_(new Impl) {}
MediaClient::~MediaClient() {
  delete impl_;
  impl_ = nullptr;
}

bool MediaClient::start(const MediaClientConfig&) {
  return false;
}

void MediaClient::stop() {}

bool MediaClient::connected() const {
  return false;
}

bool MediaClient::copy_frame_bgra(std::vector<uint8_t>&, int&, int&) const {
  return false;
}

void MediaClient::send_pointer(int, int, int) {}
bool MediaClient::send_vk(unsigned, bool) {
  return false;
}
void MediaClient::release_modifiers() {}
void MediaClient::set_software_cursor_enabled(bool) {}
void MediaClient::notify_clipboard_changed() {}
void MediaClient::set_notify_hwnd(HWND) {}

}  // namespace road_desk::media
