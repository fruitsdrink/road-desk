#pragma once

#include <string>

namespace road_desk::common {

// Escape a string for safe embedding in a JSON string value.
// Handles: " \ \b \f \n \r \t and control chars < 0x20.
std::string json_escape(const std::string& s);

// Parse a URL into components. base_path has trailing slashes stripped.
// Returns false on invalid URL.
bool parse_url(const std::string& url, std::wstring* host, unsigned short* port,
               bool* https, std::wstring* base_path);

// POST a JSON body with Bearer auth via WinHTTP. Returns true on 2xx.
// timeout_ms: 0 = OS defaults.
bool http_post_json(const std::string& base_url, const std::string& path,
                    const std::string& bearer, const std::string& json_body,
                    unsigned long timeout_ms, std::string* err);

// Generate a lowercase UUID v4 string via CoCreateGuid.
// Falls back to time-based pseudo-UUID if COM unavailable.
std::string new_session_id();

// Returns local NetBIOS computer name, or empty string on failure.
std::string local_computer_name();

}  // namespace road_desk::common
