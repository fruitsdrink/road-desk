#pragma once

#include "mux.h"
#include "mux_protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::replace {

struct FileOfferEntry {
  uint8_t flags = 0;  // kClipEntryFlagDir
  uint64_t size = 0;
  std::wstring rel_path;
  std::wstring abs_path;  // sender only
};

struct FileOffer {
  uint32_t transfer_id = 0;
  std::vector<FileOfferEntry> entries;
  std::vector<std::wstring> root_names;  // top-level HDROP names
  uint64_t total_file_bytes = 0;
  uint32_t file_count = 0;
};

// Called between chunks so Input/Control are not starved. Return false to abort.
using FileXferPumpFn = std::function<bool()>;

// Snapshot CF_HDROP into an offer (absolute paths on sender). False if empty/over limit.
bool clipboard_build_file_offer(uint32_t transfer_id, FileOffer* offer_out, std::string* err);

bool build_clip_files_offer_payload(const FileOffer& offer, std::vector<uint8_t>* out);
bool parse_clip_files_offer_payload(const uint8_t* p, size_t n, FileOffer* offer_out);

// Stream all files for offer. pump() between writes.
bool file_xfer_send_all(road_desk::media::tls::TlsSession* tls, const FileOffer& offer,
                        FileXferPumpFn pump, std::string* err);

struct FileRecvState {
  bool active = false;
  FileOffer offer;
  std::wstring stage_root;
  uint32_t files_ended = 0;
  uint32_t expect_files = 0;
  HANDLE cur_file = INVALID_HANDLE_VALUE;
  uint32_t cur_index = 0xffffffffu;
  uint64_t cur_size = 0;
  uint64_t cur_offset = 0;
  uint64_t bytes_received = 0;

  void reset();
};

// Begin staging for offer. Creates dirs from offer entries.
bool file_recv_begin(FileRecvState* st, const FileOffer& offer, std::string* err);

// Handle one File-channel payload. On XferDone success, publishes CF_HDROP.
// echo_seq_out: sequence to ignore after local SetClipboard (optional).
bool file_recv_on_payload(FileRecvState* st, const uint8_t* p, size_t n, DWORD* echo_seq_out,
                          std::string* err);

void file_recv_abort(FileRecvState* st);

// Build HDROP for absolute paths (used after staging complete).
bool clipboard_set_hdrop(const std::vector<std::wstring>& abs_paths, DWORD* echo_seq_out);

}  // namespace road_desk::replace
