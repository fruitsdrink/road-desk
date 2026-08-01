#include "mux_file_xfer.h"

#include <shellapi.h>

#include <cstdio>
#include <cstring>

// DROPFILES is in shlobj.h; declare locally under WIN32_LEAN_AND_MEAN.
struct DROPFILES {
  DWORD pFiles;
  POINT pt;
  BOOL fNC;
  BOOL fWide;
};

namespace road_desk::replace {
namespace {

void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void write_u64_le(uint8_t* p, uint64_t v) {
  write_u32_le(p, static_cast<uint32_t>(v & 0xffffffffu));
  write_u32_le(p + 4, static_cast<uint32_t>((v >> 32) & 0xffffffffu));
}

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) {
  return static_cast<uint64_t>(read_u32_le(p)) |
         (static_cast<uint64_t>(read_u32_le(p + 4)) << 32);
}

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L'\\' + b;
}

std::wstring path_file_name(const std::wstring& p) {
  const size_t slash = p.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return p;
  }
  return p.substr(slash + 1);
}

bool is_dot_or_dotdot(const wchar_t* name) {
  return name && (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0);
}

bool ensure_dir(const std::wstring& dir) {
  if (dir.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(dir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return true;
  }
  // Create parents.
  for (size_t i = 0; i < dir.size(); ++i) {
    if (dir[i] == L'\\' || dir[i] == L'/') {
      if (i == 0 || (i == 2 && dir[1] == L':')) {
        continue;
      }
      const std::wstring part = dir.substr(0, i);
      if (CreateDirectoryW(part.c_str(), nullptr) == 0) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
          // continue; leaf CreateDirectory will fail if needed
        }
      }
    }
  }
  if (CreateDirectoryW(dir.c_str(), nullptr) == 0) {
    const DWORD e = GetLastError();
    return e == ERROR_ALREADY_EXISTS;
  }
  return true;
}

void delete_tree(const std::wstring& root) {
  if (root.empty()) {
    return;
  }
  std::wstring pattern = join_path(root, L"*");
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(root.c_str());
    return;
  }
  do {
    if (is_dot_or_dotdot(fd.cFileName)) {
      continue;
    }
    const std::wstring child = join_path(root, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      delete_tree(child);
    } else {
      SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(child.c_str());
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  RemoveDirectoryW(root.c_str());
}

bool append_entry_recursive(const std::wstring& abs, const std::wstring& rel, FileOffer* offer,
                            std::string* err) {
  const DWORD attr = GetFileAttributesW(abs.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    if (err) {
      *err = "stat failed";
    }
    return false;
  }
  if (offer->entries.size() >= kMaxFileXferEntries) {
    if (err) {
      *err = "too many entries";
    }
    return false;
  }

  FileOfferEntry e;
  e.rel_path = rel;
  e.abs_path = abs;
  if (attr & FILE_ATTRIBUTE_DIRECTORY) {
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
      // Skip junctions/symlinks to avoid cycles.
      return true;
    }
    e.flags = kClipEntryFlagDir;
    e.size = 0;
    offer->entries.push_back(e);

    const std::wstring pattern = join_path(abs, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      return true;
    }
    bool ok = true;
    do {
      if (is_dot_or_dotdot(fd.cFileName)) {
        continue;
      }
      const std::wstring child_abs = join_path(abs, fd.cFileName);
      const std::wstring child_rel = join_path(rel, fd.cFileName);
      if (!append_entry_recursive(child_abs, child_rel, offer, err)) {
        ok = false;
        break;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
  }

  LARGE_INTEGER sz{};
  HANDLE hf = CreateFileW(abs.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hf == INVALID_HANDLE_VALUE) {
    if (err) {
      *err = "open file failed";
    }
    return false;
  }
  if (!GetFileSizeEx(hf, &sz)) {
    CloseHandle(hf);
    if (err) {
      *err = "size failed";
    }
    return false;
  }
  CloseHandle(hf);
  e.flags = 0;
  e.size = static_cast<uint64_t>(sz.QuadPart);
  if (offer->total_file_bytes + e.size > kMaxFileXferTotalBytes) {
    if (err) {
      *err = "xfer too large";
    }
    return false;
  }
  offer->total_file_bytes += e.size;
  ++offer->file_count;
  offer->entries.push_back(e);
  return true;
}

std::wstring make_stage_root(uint32_t transfer_id) {
  wchar_t tmp[MAX_PATH]{};
  if (GetTempPathW(MAX_PATH, tmp) == 0) {
    return {};
  }
  wchar_t buf[MAX_PATH]{};
  _snwprintf_s(buf, _TRUNCATE, L"%sRoadDesk\\clip\\%u", tmp, transfer_id);
  return buf;
}

}  // namespace

void FileRecvState::reset() {
  if (cur_file != INVALID_HANDLE_VALUE) {
    CloseHandle(cur_file);
    cur_file = INVALID_HANDLE_VALUE;
  }
  if (!stage_root.empty()) {
    delete_tree(stage_root);
    stage_root.clear();
  }
  active = false;
  offer = FileOffer{};
  files_ended = 0;
  expect_files = 0;
  cur_index = 0xffffffffu;
  cur_size = 0;
  cur_offset = 0;
  bytes_received = 0;
}

bool clipboard_build_file_offer(uint32_t transfer_id, FileOffer* offer_out, std::string* err) {
  if (!offer_out) {
    return false;
  }
  offer_out->transfer_id = transfer_id;
  offer_out->entries.clear();
  offer_out->root_names.clear();
  offer_out->total_file_bytes = 0;
  offer_out->file_count = 0;

  if (!IsClipboardFormatAvailable(CF_HDROP)) {
    if (err) {
      *err = "no hdrop";
    }
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    if (err) {
      *err = "open clipboard";
    }
    return false;
  }
  HDROP hdrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (!hdrop) {
    CloseClipboard();
    if (err) {
      *err = "get hdrop";
    }
    return false;
  }
  const UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
  std::vector<std::wstring> roots;
  roots.reserve(n);
  for (UINT i = 0; i < n; ++i) {
    const UINT need = DragQueryFileW(hdrop, i, nullptr, 0);
    std::wstring path(need, L'\0');
    if (DragQueryFileW(hdrop, i, &path[0], need + 1) == 0) {
      continue;
    }
    // DragQueryFile length excludes NUL; string may have extra NUL from resize.
    if (!path.empty() && path.back() == L'\0') {
      path.pop_back();
    }
    while (!path.empty() && (path.back() == L'\0')) {
      path.pop_back();
    }
    if (!path.empty()) {
      roots.push_back(path);
    }
  }
  CloseClipboard();

  if (roots.empty()) {
    if (err) {
      *err = "empty hdrop";
    }
    return false;
  }

  for (const std::wstring& root : roots) {
    const std::wstring name = path_file_name(root);
    if (name.empty()) {
      continue;
    }
    offer_out->root_names.push_back(name);
    if (!append_entry_recursive(root, name, offer_out, err)) {
      return false;
    }
  }
  if (offer_out->entries.empty()) {
    if (err) {
      *err = "no entries";
    }
    return false;
  }
  return true;
}

bool build_clip_files_offer_payload(const FileOffer& offer, std::vector<uint8_t>* out) {
  if (!out || offer.entries.empty()) {
    return false;
  }
  size_t need = 1 + 4 + 4;
  for (const FileOfferEntry& e : offer.entries) {
    const size_t pb = e.rel_path.size() * sizeof(wchar_t);
    if (pb > 0xffffu) {
      return false;
    }
    need += 1 + 8 + 2 + pb;
  }
  if (need > kMaxClipboardOfferBytes || need > kMaxPayloadLen) {
    return false;
  }
  out->assign(need, 0);
  size_t o = 0;
  (*out)[o++] = kClipFilesOffer;
  write_u32_le(out->data() + o, offer.transfer_id);
  o += 4;
  write_u32_le(out->data() + o, static_cast<uint32_t>(offer.entries.size()));
  o += 4;
  for (const FileOfferEntry& e : offer.entries) {
    (*out)[o++] = e.flags;
    write_u64_le(out->data() + o, e.size);
    o += 8;
    const uint16_t pb = static_cast<uint16_t>(e.rel_path.size() * sizeof(wchar_t));
    write_u16_le(out->data() + o, pb);
    o += 2;
    if (pb) {
      std::memcpy(out->data() + o, e.rel_path.data(), pb);
      o += pb;
    }
  }
  return o == need;
}

bool parse_clip_files_offer_payload(const uint8_t* p, size_t n, FileOffer* offer_out) {
  if (!p || !offer_out || n < 9 || p[0] != kClipFilesOffer) {
    return false;
  }
  size_t o = 1;
  offer_out->transfer_id = read_u32_le(p + o);
  o += 4;
  const uint32_t count = read_u32_le(p + o);
  o += 4;
  if (count == 0 || count > kMaxFileXferEntries) {
    return false;
  }
  offer_out->entries.clear();
  offer_out->root_names.clear();
  offer_out->total_file_bytes = 0;
  offer_out->file_count = 0;
  offer_out->entries.reserve(count);

  for (uint32_t i = 0; i < count; ++i) {
    if (o + 1 + 8 + 2 > n) {
      return false;
    }
    FileOfferEntry e;
    e.flags = p[o++];
    e.size = read_u64_le(p + o);
    o += 8;
    const uint16_t pb = read_u16_le(p + o);
    o += 2;
    if (o + pb > n || (pb % 2) != 0) {
      return false;
    }
    e.rel_path.assign(reinterpret_cast<const wchar_t*>(p + o), pb / sizeof(wchar_t));
    o += pb;
    if (e.rel_path.find(L"..") != std::wstring::npos) {
      return false;
    }
    if (e.flags & kClipEntryFlagDir) {
      e.size = 0;
    } else {
      offer_out->total_file_bytes += e.size;
      ++offer_out->file_count;
    }
    if (offer_out->total_file_bytes > kMaxFileXferTotalBytes) {
      return false;
    }
    // Top-level: no backslash in relative path.
    if (e.rel_path.find(L'\\') == std::wstring::npos &&
        e.rel_path.find(L'/') == std::wstring::npos) {
      offer_out->root_names.push_back(e.rel_path);
    }
    offer_out->entries.push_back(std::move(e));
  }
  return o == n;
}

bool file_xfer_send_all(road_desk::media::tls::TlsSession* tls, const FileOffer& offer,
                        FileXferPumpFn pump, std::string* err) {
  if (!tls) {
    return false;
  }
  std::vector<uint8_t> body;
  body.reserve(32 + kMaxFileChunkBytes);

  for (uint32_t i = 0; i < offer.entries.size(); ++i) {
    if (pump && !pump()) {
      if (err) {
        *err = "pump abort";
      }
      return false;
    }
    const FileOfferEntry& e = offer.entries[i];
    if (e.flags & kClipEntryFlagDir) {
      continue;
    }
    const uint16_t pb = static_cast<uint16_t>(e.rel_path.size() * sizeof(wchar_t));
    body.resize(1 + 4 + 4 + 8 + 2 + pb);
    body[0] = kFileBegin;
    write_u32_le(body.data() + 1, offer.transfer_id);
    write_u32_le(body.data() + 5, i);
    write_u64_le(body.data() + 9, e.size);
    write_u16_le(body.data() + 17, pb);
    if (pb) {
      std::memcpy(body.data() + 19, e.rel_path.data(), pb);
    }
    if (!mux_write(tls, kChannelFile, body.data(), static_cast<uint32_t>(body.size()))) {
      if (err) {
        *err = "begin write";
      }
      return false;
    }

    HANDLE hf = CreateFileW(e.abs_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
      if (err) {
        *err = "open for send";
      }
      return false;
    }

    uint64_t offset = 0;
    std::vector<uint8_t> chunk(kMaxFileChunkBytes);
    while (offset < e.size) {
      if (pump && !pump()) {
        CloseHandle(hf);
        if (err) {
          *err = "pump abort";
        }
        return false;
      }
      DWORD to_read = kMaxFileChunkBytes;
      if (e.size - offset < to_read) {
        to_read = static_cast<DWORD>(e.size - offset);
      }
      DWORD got = 0;
      if (!ReadFile(hf, chunk.data(), to_read, &got, nullptr) || got == 0) {
        CloseHandle(hf);
        if (err) {
          *err = "read file";
        }
        return false;
      }
      body.resize(1 + 4 + 4 + 8 + 4 + got);
      body[0] = kFileChunk;
      write_u32_le(body.data() + 1, offer.transfer_id);
      write_u32_le(body.data() + 5, i);
      write_u64_le(body.data() + 9, offset);
      write_u32_le(body.data() + 17, got);
      std::memcpy(body.data() + 21, chunk.data(), got);
      if (!mux_write(tls, kChannelFile, body.data(), static_cast<uint32_t>(body.size()))) {
        CloseHandle(hf);
        if (err) {
          *err = "chunk write";
        }
        return false;
      }
      offset += got;
    }
    CloseHandle(hf);

    body.resize(1 + 4 + 4);
    body[0] = kFileEnd;
    write_u32_le(body.data() + 1, offer.transfer_id);
    write_u32_le(body.data() + 5, i);
    if (!mux_write(tls, kChannelFile, body.data(), static_cast<uint32_t>(body.size()))) {
      if (err) {
        *err = "end write";
      }
      return false;
    }
  }

  if (pump && !pump()) {
    return false;
  }
  body.resize(1 + 4);
  body[0] = kFileXferDone;
  write_u32_le(body.data() + 1, offer.transfer_id);
  if (!mux_write(tls, kChannelFile, body.data(), static_cast<uint32_t>(body.size()))) {
    if (err) {
      *err = "done write";
    }
    return false;
  }
  return true;
}

bool clipboard_set_hdrop(const std::vector<std::wstring>& abs_paths, DWORD* echo_seq_out) {
  if (abs_paths.empty()) {
    return false;
  }
  size_t chars = 1;  // final double-NUL
  for (const std::wstring& p : abs_paths) {
    chars += p.size() + 1;
  }
  const SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GHND, bytes);
  if (!h) {
    return false;
  }
  auto* df = static_cast<DROPFILES*>(GlobalLock(h));
  if (!df) {
    GlobalFree(h);
    return false;
  }
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
  for (const std::wstring& p : abs_paths) {
    std::memcpy(dest, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
    dest += p.size() + 1;
  }
  *dest = L'\0';
  GlobalUnlock(h);

  if (!OpenClipboard(nullptr)) {
    GlobalFree(h);
    return false;
  }
  EmptyClipboard();
  if (!SetClipboardData(CF_HDROP, h)) {
    GlobalFree(h);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  if (echo_seq_out) {
    *echo_seq_out = GetClipboardSequenceNumber();
  }
  return true;
}

bool file_recv_begin(FileRecvState* st, const FileOffer& offer, std::string* err) {
  if (!st) {
    return false;
  }
  file_recv_abort(st);
  st->offer = offer;
  st->expect_files = offer.file_count;
  st->files_ended = 0;
  st->stage_root = make_stage_root(offer.transfer_id);
  if (st->stage_root.empty()) {
    if (err) {
      *err = "temp path";
    }
    return false;
  }
  delete_tree(st->stage_root);
  if (!ensure_dir(st->stage_root)) {
    if (err) {
      *err = "mkdir stage";
    }
    return false;
  }
  for (const FileOfferEntry& e : offer.entries) {
    if (e.flags & kClipEntryFlagDir) {
      if (!ensure_dir(join_path(st->stage_root, e.rel_path))) {
        if (err) {
          *err = "mkdir entry";
        }
        file_recv_abort(st);
        return false;
      }
    }
  }
  // Deduplicate root names (offer parser may add file roots and dir roots).
  std::vector<std::wstring> roots;
  for (const FileOfferEntry& e : offer.entries) {
    if (e.rel_path.find(L'\\') == std::wstring::npos &&
        e.rel_path.find(L'/') == std::wstring::npos) {
      bool have = false;
      for (const std::wstring& r : roots) {
        if (r == e.rel_path) {
          have = true;
          break;
        }
      }
      if (!have) {
        roots.push_back(e.rel_path);
      }
    }
  }
  st->offer.root_names = std::move(roots);
  st->active = true;
  return true;
}

void file_recv_abort(FileRecvState* st) {
  if (!st) {
    return;
  }
  st->reset();
}

bool file_recv_on_payload(FileRecvState* st, const uint8_t* p, size_t n, DWORD* echo_seq_out,
                          std::string* err) {
  if (!st || !p || n < 1) {
    return false;
  }
  const uint8_t type = p[0];
  if (type == kFileAbort) {
    if (n < 5) {
      return false;
    }
    file_recv_abort(st);
    return true;
  }
  if (!st->active) {
    return true;  // ignore stray
  }

  if (type == kFileBegin) {
    if (n < 19) {
      return false;
    }
    const uint32_t xid = read_u32_le(p + 1);
    const uint32_t idx = read_u32_le(p + 5);
    const uint64_t size = read_u64_le(p + 9);
    const uint16_t pb = read_u16_le(p + 17);
    if (xid != st->offer.transfer_id || idx >= st->offer.entries.size() || n < 19u + pb) {
      return false;
    }
    if (st->cur_file != INVALID_HANDLE_VALUE) {
      CloseHandle(st->cur_file);
      st->cur_file = INVALID_HANDLE_VALUE;
    }
    const FileOfferEntry& e = st->offer.entries[idx];
    if (e.flags & kClipEntryFlagDir) {
      return false;
    }
    const std::wstring full = join_path(st->stage_root, e.rel_path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      ensure_dir(full.substr(0, slash));
    }
    HANDLE hf = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
      if (err) {
        *err = "create staged file";
      }
      return false;
    }
    st->cur_file = hf;
    st->cur_index = idx;
    st->cur_size = size;
    st->cur_offset = 0;
    return true;
  }

  if (type == kFileChunk) {
    if (n < 21 || st->cur_file == INVALID_HANDLE_VALUE) {
      return false;
    }
    const uint32_t xid = read_u32_le(p + 1);
    const uint32_t idx = read_u32_le(p + 5);
    const uint64_t offset = read_u64_le(p + 9);
    const uint32_t len = read_u32_le(p + 17);
    if (xid != st->offer.transfer_id || idx != st->cur_index || offset != st->cur_offset ||
        n < 21u + len || len > kMaxFileChunkBytes) {
      return false;
    }
    if (st->bytes_received + len > kMaxFileXferTotalBytes) {
      if (err) {
        *err = "recv over limit";
      }
      return false;
    }
    DWORD wrote = 0;
    if (!WriteFile(st->cur_file, p + 21, len, &wrote, nullptr) || wrote != len) {
      if (err) {
        *err = "write staged";
      }
      return false;
    }
    st->cur_offset += len;
    st->bytes_received += len;
    return true;
  }

  if (type == kFileEnd) {
    if (n < 9 || st->cur_file == INVALID_HANDLE_VALUE) {
      return false;
    }
    const uint32_t xid = read_u32_le(p + 1);
    const uint32_t idx = read_u32_le(p + 5);
    if (xid != st->offer.transfer_id || idx != st->cur_index) {
      return false;
    }
    if (st->cur_offset != st->cur_size) {
      if (err) {
        *err = "size mismatch";
      }
      return false;
    }
    CloseHandle(st->cur_file);
    st->cur_file = INVALID_HANDLE_VALUE;
    st->cur_index = 0xffffffffu;
    ++st->files_ended;
    return true;
  }

  if (type == kFileXferDone) {
    if (n < 5) {
      return false;
    }
    const uint32_t xid = read_u32_le(p + 1);
    if (xid != st->offer.transfer_id) {
      return false;
    }
    if (st->files_ended != st->expect_files) {
      if (err) {
        *err = "incomplete xfer";
      }
      file_recv_abort(st);
      return false;
    }
    std::vector<std::wstring> roots;
    roots.reserve(st->offer.root_names.size());
    for (const std::wstring& name : st->offer.root_names) {
      roots.push_back(join_path(st->stage_root, name));
    }
    // Keep staging for paste; clear handles but not tree.
    if (st->cur_file != INVALID_HANDLE_VALUE) {
      CloseHandle(st->cur_file);
      st->cur_file = INVALID_HANDLE_VALUE;
    }
    const std::wstring keep_root = st->stage_root;
    st->active = false;
    st->stage_root.clear();  // ownership transferred to clipboard lifetime
    if (!clipboard_set_hdrop(roots, echo_seq_out)) {
      delete_tree(keep_root);
      if (err) {
        *err = "set hdrop";
      }
      return false;
    }
    // Note: staged files intentionally kept until next xfer overwrites same id / session end.
    // Re-store root for session cleanup.
    st->stage_root = keep_root;
    return true;
  }

  return true;
}

}  // namespace road_desk::replace
