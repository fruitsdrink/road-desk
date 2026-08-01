#pragma once

#include "media_plane.h"

#include <string>

namespace road_desk::viewer {

// Demo / default connect target (all tree devices share this).
// Override: ROAD_DESK_DEMO_HOST, ROAD_DESK_DEMO_PORT, ROAD_DESK_PSK,
//           ROAD_DESK_TLS_FINGERPRINT, ROAD_DESK_TLS_INSECURE.
struct ConnectDefaults {
  std::string host_port = "192.168.26.131:38471";
  std::string password = "road-desk";
  std::string tls_fingerprint;
  bool tls_insecure = false;
  bool require_tls = true;
};

ConnectDefaults load_connect_defaults();
void apply_connect_defaults_to(road_desk::media::MediaClientConfig* cfg,
                               const ConnectDefaults& d);

// Parse "host:port [password] [fingerprint]" from narrow command line.
bool parse_direct_args(const char* narrow_cmd, ConnectDefaults* out);

}  // namespace road_desk::viewer
