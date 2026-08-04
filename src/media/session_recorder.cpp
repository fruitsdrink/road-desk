#include "session_recorder.h"

#include "media_log.h"
#include "product_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vfw.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>
#include <oleauto.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace road_desk::media {
namespace {

template <typename T>
void safe_release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

enum class RecBackend : uint8_t { None = 0, Mf = 1, Avi = 2 };

struct RecState {
  RecBackend backend = RecBackend::None;
  IMFSinkWriter* writer = nullptr;
  DWORD stream = 0;
  PAVIFILE avi_file = nullptr;
  PAVISTREAM avi_stream = nullptr;
  LONG avi_sample = 0;
  bool avi_inited = false;
  int src_w = 0;
  int src_h = 0;
  int w = 0;
  int h = 0;
  LONGLONG rts = 0;
  DWORD last_push_ms = 0;
  uint32_t frames_written = 0;
  uint32_t encode_fail = 0;
  uint32_t write_fail = 0;
  bool logged_push_fail = false;
  std::wstring video_wpath;
  std::string video_path;
  std::string session_id;
  bool mf_started = false;
  bool com_owned = false;
};

RecState g_rec;
bool g_unavailable = false;
constexpr DWORD kMinFrameMs = 100;  // ~10 fps
constexpr int kAviJpegQuality = 55;

std::string exe_dir() {
  char buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return ".";
  }
  char* slash = std::strrchr(buf, '\\');
  if (!slash) {
    slash = std::strrchr(buf, '/');
  }
  if (slash) {
    *slash = '\0';
  }
  return buf;
}

std::string make_session_id() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char id[64] = {};
  std::snprintf(id, sizeof(id), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                static_cast<unsigned>(st.wSecond));
  return id;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  }
  return w;
}

bool ensure_dir(const std::string& path) {
  return CreateDirectoryA(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool is_session_artifact(const char* name) {
  if (!name || !name[0]) {
    return false;
  }
  if (std::strncmp(name, "session_", 8) == 0) {
    return std::strstr(name, ".mp4") != nullptr || std::strstr(name, ".avi") != nullptr;
  }
  return std::strncmp(name, "host-agent_", 11) == 0 && std::strstr(name, ".log") != nullptr;
}

void prune_debug_artifacts(const std::string& keep_session_id) {
  const std::string dir = session_recorder_debug_dir();
  const std::string keep_mp4 =
      keep_session_id.empty() ? std::string() : ("session_" + keep_session_id + ".mp4");
  const std::string keep_avi =
      keep_session_id.empty() ? std::string() : ("session_" + keep_session_id + ".avi");
  const std::string keep_log =
      keep_session_id.empty() ? std::string() : ("host-agent_" + keep_session_id + ".log");

  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    if (!is_session_artifact(fd.cFileName)) {
      continue;
    }
    if ((!keep_mp4.empty() && keep_mp4 == fd.cFileName) ||
        (!keep_avi.empty() && keep_avi == fd.cFileName) ||
        (!keep_log.empty() && keep_log == fd.cFileName)) {
      continue;
    }
    DeleteFileA((dir + "\\" + fd.cFileName).c_str());
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

bool copy_log_snapshot(const std::string& dest) {
  const char* src = media_log_path();
  if (!src || !src[0]) {
    return false;
  }
  return CopyFileA(src, dest.c_str(), FALSE) != 0;
}

std::string basename_of(const std::string& path) {
  const size_t a = path.find_last_of('\\');
  const size_t b = path.find_last_of('/');
  size_t pos = std::string::npos;
  if (a != std::string::npos) {
    pos = a;
  }
  if (b != std::string::npos && (pos == std::string::npos || b > pos)) {
    pos = b;
  }
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

void write_latest_manifest(const std::string& video, const std::string& log_copy,
                           const std::string& session_id) {
  const std::string path = session_recorder_debug_dir() + "\\latest.json";
  FILE* fp = nullptr;
  if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
    return;
  }
  auto esc = [](const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
      if (c == '\\' || c == '"') {
        o.push_back('\\');
      }
      o.push_back(c);
    }
    return o;
  };
  std::fprintf(fp,
               "{\n"
               "  \"session_id\": \"%s\",\n"
               "  \"version\": \"%s\",\n"
               "  \"built\": \"%s %s\",\n"
               "  \"video\": \"%s\",\n"
               "  \"log\": \"%s\",\n"
               "  \"video_path\": \"%s\",\n"
               "  \"log_path\": \"%s\",\n"
               "  \"dir\": \"%s\"\n"
               "}\n",
               esc(session_id).c_str(), ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE,
               ROAD_DESK_BUILD_TIME, esc(basename_of(video)).c_str(),
               esc(basename_of(log_copy)).c_str(), esc(video).c_str(), esc(log_copy).c_str(),
               esc(session_recorder_debug_dir()).c_str());
  std::fclose(fp);
}

void shutdown_mf_com() {
  if (g_rec.mf_started) {
    MFShutdown();
    g_rec.mf_started = false;
  }
}

void shutdown_com_if_owned() {
  if (g_rec.com_owned) {
    CoUninitialize();
    g_rec.com_owned = false;
  }
}

bool configure_writer(IMFSinkWriter* writer, const GUID& subtype, int width, int height,
                      DWORD* stream_out) {
  IMFMediaType* out_type = nullptr;
  MFCreateMediaType(&out_type);
  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_type->SetGUID(MF_MT_SUBTYPE, subtype);
  out_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(width * height * 2));
  out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                     static_cast<UINT32>(height));
  MFSetAttributeRatio(out_type, MF_MT_FRAME_RATE, 10, 1);
  MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  DWORD stream = 0;
  HRESULT hr = writer->AddStream(out_type, &stream);
  safe_release(out_type);
  if (FAILED(hr)) {
    return false;
  }

  IMFMediaType* in_type = nullptr;
  MFCreateMediaType(&in_type);
  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
  MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                     static_cast<UINT32>(height));
  MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, 10, 1);
  MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  hr = writer->SetInputMediaType(stream, in_type, nullptr);
  safe_release(in_type);
  if (FAILED(hr)) {
    return false;
  }
  if (FAILED(writer->BeginWriting())) {
    return false;
  }
  *stream_out = stream;
  return true;
}

bool encode_bgra_jpeg(const uint8_t* bgra, int width, int height, int quality,
                      std::vector<uint8_t>* out) {
  if (!bgra || !out || width < 1 || height < 1) {
    return false;
  }
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }

  // Prefer WIC1: with Win8+ SDKs CLSID_WICImagingFactory aliases Factory2, which
  // is missing on Win7 and makes every encode fail (0-frame AVI → deleted).
  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) {
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
  }
  if (FAILED(hr) || !factory) {
    return false;
  }

  const UINT stride = static_cast<UINT>(width) * 4u;
  const UINT cb = stride * static_cast<UINT>(height);
  IWICBitmap* bitmap = nullptr;
  hr = factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                       GUID_WICPixelFormat32bppBGRA, stride, cb,
                                       const_cast<BYTE*>(bgra), &bitmap);
  if (FAILED(hr) || !bitmap) {
    safe_release(factory);
    return false;
  }

  IStream* stream = nullptr;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (FAILED(hr) || !stream) {
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  IWICBitmapEncoder* encoder = nullptr;
  hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  if (FAILED(hr) || !encoder || FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  hr = encoder->CreateNewFrame(&frame, &props);
  if (FAILED(hr) || !frame) {
    safe_release(props);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  if (props) {
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_R4;
    var.fltVal = static_cast<FLOAT>(quality) / 100.0f;
    props->Write(1, &opt, &var);
    VariantClear(&var);
  }

  hr = frame->Initialize(props);
  safe_release(props);
  if (FAILED(hr)) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) ||
      FAILED(frame->SetPixelFormat(&fmt)) || FAILED(frame->WriteSource(bitmap, nullptr)) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  safe_release(frame);
  safe_release(encoder);
  safe_release(bitmap);
  safe_release(factory);

  STATSTG st{};
  if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart <= 0 ||
      st.cbSize.QuadPart > 32 * 1024 * 1024) {
    safe_release(stream);
    return false;
  }
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  out->resize(static_cast<size_t>(st.cbSize.QuadPart));
  ULONG got = 0;
  hr = stream->Read(out->data(), static_cast<ULONG>(out->size()), &got);
  safe_release(stream);
  if (FAILED(hr) || got != out->size()) {
    out->clear();
    return false;
  }
  return true;
}

void close_avi() {
  if (g_rec.avi_stream) {
    AVIStreamRelease(g_rec.avi_stream);
    g_rec.avi_stream = nullptr;
  }
  if (g_rec.avi_file) {
    AVIFileRelease(g_rec.avi_file);
    g_rec.avi_file = nullptr;
  }
  if (g_rec.avi_inited) {
    AVIFileExit();
    g_rec.avi_inited = false;
  }
}

bool begin_avi(int width, int height) {
  const std::string dir = session_recorder_debug_dir();
  g_rec.video_path = dir + "\\session_" + g_rec.session_id + ".avi";
  g_rec.video_wpath = utf8_to_wide(g_rec.video_path);
  DeleteFileA(g_rec.video_path.c_str());

  AVIFileInit();
  g_rec.avi_inited = true;

  PAVIFILE file = nullptr;
  HRESULT hr = AVIFileOpenW(&file, g_rec.video_wpath.c_str(), OF_WRITE | OF_CREATE, nullptr);
  if (FAILED(hr) || !file) {
    close_avi();
    return false;
  }

  AVISTREAMINFOW asi{};
  asi.fccType = streamtypeVIDEO;
  asi.fccHandler = mmioFOURCC('M', 'J', 'P', 'G');
  asi.dwScale = 1;
  asi.dwRate = 10;
  asi.dwSuggestedBufferSize = static_cast<DWORD>(width) * height;
  asi.rcFrame.left = 0;
  asi.rcFrame.top = 0;
  asi.rcFrame.right = width;
  asi.rcFrame.bottom = height;
  asi.dwQuality = static_cast<DWORD>(-1);

  PAVISTREAM stream = nullptr;
  hr = AVIFileCreateStreamW(file, &stream, &asi);
  if (FAILED(hr) || !stream) {
    AVIFileRelease(file);
    close_avi();
    return false;
  }

  BITMAPINFOHEADER bih{};
  bih.biSize = sizeof(bih);
  bih.biWidth = width;
  bih.biHeight = height;
  bih.biPlanes = 1;
  bih.biBitCount = 24;
  bih.biCompression = mmioFOURCC('M', 'J', 'P', 'G');
  bih.biSizeImage = static_cast<DWORD>(width) * height * 3u;
  hr = AVIStreamSetFormat(stream, 0, &bih, sizeof(bih));
  if (FAILED(hr)) {
    AVIStreamRelease(stream);
    AVIFileRelease(file);
    close_avi();
    return false;
  }

  g_rec.avi_file = file;
  g_rec.avi_stream = stream;
  g_rec.avi_sample = 0;
  g_rec.backend = RecBackend::Avi;
  media_logf("session-record", "avi fallback %dx%d %s", width, height, g_rec.video_path.c_str());
  return true;
}

bool try_begin_mf(int width, int height) {
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
    return false;
  }
  g_rec.mf_started = true;

  const std::string dir = session_recorder_debug_dir();
  g_rec.video_path = dir + "\\session_" + g_rec.session_id + ".mp4";
  g_rec.video_wpath = utf8_to_wide(g_rec.video_path);

  IMFAttributes* attrs = nullptr;
  MFCreateAttributes(&attrs, 2);
  if (attrs) {
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  }

  IMFSinkWriter* writer = nullptr;
  HRESULT hr = MFCreateSinkWriterFromURL(g_rec.video_wpath.c_str(), nullptr, attrs, &writer);
  safe_release(attrs);
  if (FAILED(hr) || !writer) {
    shutdown_mf_com();
    return false;
  }

  DWORD stream = 0;
  if (!configure_writer(writer, MFVideoFormat_H264, width, height, &stream)) {
    safe_release(writer);
    hr = MFCreateSinkWriterFromURL(g_rec.video_wpath.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr) || !writer ||
        !configure_writer(writer, MFVideoFormat_MJPG, width, height, &stream)) {
      safe_release(writer);
      DeleteFileA(g_rec.video_path.c_str());
      shutdown_mf_com();
      return false;
    }
  }

  g_rec.writer = writer;
  g_rec.stream = stream;
  g_rec.backend = RecBackend::Mf;
  return true;
}

void crop_top_left(const uint8_t* src, int src_w, int ew, int eh, std::vector<uint8_t>* dst) {
  dst->resize(static_cast<size_t>(ew) * eh * 4u);
  const int src_stride = src_w * 4;
  const int dst_stride = ew * 4;
  for (int y = 0; y < eh; ++y) {
    std::memcpy(dst->data() + static_cast<size_t>(y) * dst_stride,
                src + static_cast<size_t>(y) * src_stride, static_cast<size_t>(dst_stride));
  }
}

}  // namespace

bool session_recorder_enabled() {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, "ROAD_DESK_AUTO_RECORD") != 0 || !env) {
    return true;
  }
  const bool off = (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' ||
                    env[0] == 'F');
  free(env);
  return !off;
}

std::string session_recorder_debug_dir() {
  return exe_dir() + "\\debug";
}

bool session_recorder_active() {
  return g_rec.backend != RecBackend::None;
}

void session_recorder_reset() {
  session_recorder_end();
  g_unavailable = false;
}

bool session_recorder_begin(int width, int height, SessionRecorderInfo* info) {
  if (info) {
    *info = {};
  }
  if (g_unavailable || g_rec.backend != RecBackend::None) {
    return g_rec.backend != RecBackend::None;
  }
  if (!session_recorder_enabled() || width < 16 || height < 16) {
    return false;
  }

  const int src_w = width;
  const int src_h = height;
  width &= ~1;
  height &= ~1;
  if (width < 16 || height < 16) {
    g_unavailable = true;
    return false;
  }

  const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  g_rec.com_owned = (chr == S_OK);
  if (FAILED(chr) && chr != S_FALSE && chr != RPC_E_CHANGED_MODE) {
    g_unavailable = true;
    return false;
  }

  const std::string dir = session_recorder_debug_dir();
  if (!ensure_dir(dir)) {
    shutdown_com_if_owned();
    g_rec = {};
    g_unavailable = true;
    return false;
  }

  prune_debug_artifacts(/*keep_session_id=*/"");
  g_rec.session_id = make_session_id();
  g_rec.src_w = src_w;
  g_rec.src_h = src_h;
  g_rec.w = width;
  g_rec.h = height;
  g_rec.rts = 0;
  g_rec.last_push_ms = 0;
  g_rec.frames_written = 0;

  bool ok = try_begin_mf(width, height);
  if (!ok) {
    ok = begin_avi(width, height);
  }
  if (!ok) {
    shutdown_mf_com();
    shutdown_com_if_owned();
    g_rec = {};
    g_unavailable = true;
    return false;
  }

  {
    const std::string log_expect = dir + "\\host-agent_" + g_rec.session_id + ".log";
    write_latest_manifest(g_rec.video_path, log_expect, g_rec.session_id);
  }
  if (info) {
    info->video_path = g_rec.video_path;
    info->log_copy_path = dir + "\\host-agent_" + g_rec.session_id + ".log";
    info->manifest_path = dir + "\\latest.json";
    info->active = true;
  }
  return true;
}

void session_recorder_push_bgra(const uint8_t* bgra, int width, int height) {
  if (g_rec.backend == RecBackend::None || !bgra || width != g_rec.src_w ||
      height != g_rec.src_h) {
    return;
  }
  const DWORD now = GetTickCount();
  if (g_rec.last_push_ms != 0 && now - g_rec.last_push_ms < kMinFrameMs) {
    return;
  }
  g_rec.last_push_ms = now;

  const int ew = g_rec.w;
  const int eh = g_rec.h;
  std::vector<uint8_t> cropped;
  crop_top_left(bgra, width, ew, eh, &cropped);

  if (g_rec.backend == RecBackend::Mf) {
    const DWORD cb = static_cast<DWORD>(ew) * eh * 4u;
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    if (FAILED(MFCreateMemoryBuffer(cb, &buffer)) || FAILED(MFCreateSample(&sample))) {
      safe_release(buffer);
      safe_release(sample);
      return;
    }
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr)) || !dst) {
      safe_release(buffer);
      safe_release(sample);
      return;
    }
    std::memcpy(dst, cropped.data(), cb);
    buffer->Unlock();
    buffer->SetCurrentLength(cb);
    sample->AddBuffer(buffer);
    safe_release(buffer);
    sample->SetSampleTime(g_rec.rts);
    sample->SetSampleDuration(1000000);
    g_rec.rts += 1000000;
    if (SUCCEEDED(g_rec.writer->WriteSample(g_rec.stream, sample))) {
      ++g_rec.frames_written;
    }
    safe_release(sample);
    return;
  }

  // AVI / MJPEG fallback (Win7).
  std::vector<uint8_t> jpeg;
  if (!encode_bgra_jpeg(cropped.data(), ew, eh, kAviJpegQuality, &jpeg) || jpeg.empty()) {
    ++g_rec.encode_fail;
    if (!g_rec.logged_push_fail) {
      g_rec.logged_push_fail = true;
      media_logf("session-record", "avi jpeg encode failed (will retry)");
    }
    return;
  }
  const HRESULT hr =
      AVIStreamWrite(g_rec.avi_stream, g_rec.avi_sample, 1, jpeg.data(),
                     static_cast<LONG>(jpeg.size()), AVIIF_KEYFRAME, nullptr, nullptr);
  if (SUCCEEDED(hr)) {
    ++g_rec.avi_sample;
    ++g_rec.frames_written;
  } else {
    ++g_rec.write_fail;
    if (!g_rec.logged_push_fail) {
      g_rec.logged_push_fail = true;
      media_logf("session-record", "avi write failed hr=0x%08lx",
                 static_cast<unsigned long>(hr));
    }
  }
}

void session_recorder_end() {
  const std::string video = g_rec.video_path;
  const std::string sid = g_rec.session_id;
  const uint32_t frames = g_rec.frames_written;
  const uint32_t enc_fail = g_rec.encode_fail;
  const uint32_t wr_fail = g_rec.write_fail;
  const RecBackend backend = g_rec.backend;
  std::string log_copy;
  if (!sid.empty()) {
    log_copy = session_recorder_debug_dir() + "\\host-agent_" + sid + ".log";
  }

  if (g_rec.backend == RecBackend::Mf && g_rec.writer) {
    g_rec.writer->Finalize();
    safe_release(g_rec.writer);
  }
  if (g_rec.backend == RecBackend::Avi) {
    close_avi();
  }

  std::string video_kept = video;
  if (!video.empty() && frames == 0) {
    DeleteFileA(video.c_str());
    video_kept.clear();
    media_logf("session-record", "discarded empty video backend=%u enc_fail=%u write_fail=%u",
               static_cast<unsigned>(backend), enc_fail, wr_fail);
  } else if (!video_kept.empty()) {
    media_logf("session-record", "finalized frames=%u backend=%u %s", frames,
               static_cast<unsigned>(backend), video_kept.c_str());
  }

  if (!log_copy.empty()) {
    copy_log_snapshot(log_copy);
  }
  if (!video_kept.empty() || !log_copy.empty()) {
    write_latest_manifest(video_kept, log_copy, sid);
    prune_debug_artifacts(sid);
  }

  shutdown_mf_com();
  shutdown_com_if_owned();
  g_rec = {};
}

}  // namespace road_desk::media
