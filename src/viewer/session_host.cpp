#include "session_host.h"

#include "audit_client.h"
#include "media_log.h"
#include "ui/rd_app_icon.h"
#include "ui/rd_dpi.h"
#include "ui/rd_tokens.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace road_desk::viewer {
namespace gdip = Gdiplus;
namespace {

namespace rd = road_desk::ui;

constexpr wchar_t kSessionClass[] = L"RoadDeskSessionHost";
constexpr UINT_PTR kReconnectTimerId = 42;

SessionHost* g_kb_target = nullptr;
HHOOK g_kb_hook = nullptr;

// Aspect-preserving fit: scale (up or down) so the frame fills the client as much
// as possible. Letterbox only when client and framebuffer aspect ratios differ.
bool fit_rect(int cw, int ch, int fb_w, int fb_h, RECT* out, bool /*fit_workspace*/) {
  if (cw <= 0 || ch <= 0 || fb_w <= 0 || fb_h <= 0 || !out) {
    return false;
  }
  const double sx = static_cast<double>(cw) / fb_w;
  const double sy = static_cast<double>(ch) / fb_h;
  const double s = (sx < sy) ? sx : sy;
  int dw = static_cast<int>(fb_w * s + 0.5);
  int dh = static_cast<int>(fb_h * s + 0.5);
  if (dw < 1) {
    dw = 1;
  }
  if (dh < 1) {
    dh = 1;
  }
  if (dw > cw) {
    dw = cw;
  }
  if (dh > ch) {
    dh = ch;
  }
  out->left = (cw - dw) / 2;
  out->top = (ch - dh) / 2;
  out->right = out->left + dw;
  out->bottom = out->top + dh;
  return true;
}

bool client_point_in_letterbox(HWND hwnd, LPARAM lparam, int fb_w, int fb_h,
                               bool fit_workspace) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest, fit_workspace)) {
    return false;
  }
  const int cx = GET_X_LPARAM(lparam);
  const int cy = GET_Y_LPARAM(lparam);
  return cx >= dest.left && cx < dest.right && cy >= dest.top && cy < dest.bottom;
}

int map_mouse_x(HWND hwnd, LPARAM lparam, int fb_w, int fb_h, bool fit_workspace) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest, fit_workspace)) {
    return 0;
  }
  const int dw = dest.right - dest.left;
  if (dw <= 0) {
    return 0;
  }
  int x = ((GET_X_LPARAM(lparam) - dest.left) * fb_w) / dw;
  if (x < 0) {
    x = 0;
  }
  if (x >= fb_w) {
    x = fb_w - 1;
  }
  return x;
}

int map_mouse_y(HWND hwnd, LPARAM lparam, int fb_w, int fb_h, bool fit_workspace) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest, fit_workspace)) {
    return 0;
  }
  const int dh = dest.bottom - dest.top;
  if (dh <= 0) {
    return 0;
  }
  int y = ((GET_Y_LPARAM(lparam) - dest.top) * fb_h) / dh;
  if (y < 0) {
    y = 0;
  }
  if (y >= fb_h) {
    y = fb_h - 1;
  }
  return y;
}

LRESULT CALLBACK low_level_keyboard(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_kb_target && g_kb_target->connected()) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (!(info->flags & LLKHF_INJECTED)) {
      // Esc always exits fullscreen locally instead of reaching the remote.
      if (g_kb_target->fullscreen() && info->vkCode == VK_ESCAPE &&
          wparam == WM_KEYDOWN) {
        g_kb_target->toggle_fullscreen();
        return 1;
      }
      if (g_kb_target->wants_keyboard()) {
        const bool down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
        if (g_kb_target->send_vk(info->vkCode, down)) {
          return 1;
        }
      }
    }
  }
  return CallNextHookEx(g_kb_hook, code, wparam, lparam);
}

}  // namespace

LRESULT CALLBACK SessionWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  SessionHost* self = SessionHost::from_hwnd(hwnd);
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SessionHost*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) {
      self->hwnd_ = hwnd;
    }
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
    case WM_MEDIA_RESIZE: {
      // Show the version the connected agent reported in the handshake so a
      // stale gateway-cached version can not be mistaken for the live agent.
      const std::string agent_ver = self->client_.agent_version();
      std::wstring ver_suffix;
      if (!agent_ver.empty()) {
        wchar_t wv[64] = L"";
        MultiByteToWideChar(CP_UTF8, 0, agent_ver.c_str(), -1, wv, 64);
        ver_suffix = L" [agent ";
        ver_suffix += wv;
        ver_suffix += L"]";
      }
      if (self->floating_) {
        wchar_t title[192];
        _snwprintf_s(title, _TRUNCATE, L"%s (%dx%d)%s", self->title_.c_str(),
                     static_cast<int>(wparam), static_cast<int>(lparam),
                     ver_suffix.c_str());
        SetWindowTextW(hwnd, title);
      }
      // Docked tab: ask the console to refresh labels with the agent version.
      PostMessageW(GetAncestor(hwnd, GA_ROOT), WM_SESSION_META, 0, 0);
      self->update_scrollbars();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      self->update_scrollbars();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_HSCROLL: {
      int fb_w = 0;
      int fb_h = 0;
      if (!self->client_.framebuffer_size(&fb_w, &fb_h) || fb_w <= 0 || fb_h <= 0) {
        return 0;
      }
      RECT crc{};
      GetClientRect(hwnd, &crc);
      const int ccw = crc.right - crc.left;
      const int cch = crc.bottom - crc.top;
      const int max_x = (fb_w > ccw) ? (fb_w - ccw) : 0;
      switch (LOWORD(wparam)) {
        case SB_LINELEFT:
          self->scroll_x_ -= 20;
          break;
        case SB_LINERIGHT:
          self->scroll_x_ += 20;
          break;
        case SB_PAGELEFT:
          self->scroll_x_ -= ccw / 2;
          break;
        case SB_PAGERIGHT:
          self->scroll_x_ += ccw / 2;
          break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
          self->scroll_x_ = static_cast<int>(HIWORD(wparam));
          break;
        default:
          break;
      }
      if (self->scroll_x_ < 0) {
        self->scroll_x_ = 0;
      }
      if (self->scroll_x_ > max_x) {
        self->scroll_x_ = max_x;
      }
      self->update_scrollbars();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_VSCROLL: {
      int fb_w = 0;
      int fb_h = 0;
      if (!self->client_.framebuffer_size(&fb_w, &fb_h) || fb_w <= 0 || fb_h <= 0) {
        return 0;
      }
      RECT crc{};
      GetClientRect(hwnd, &crc);
      const int cch = crc.bottom - crc.top;
      const int max_y = (fb_h > cch) ? (fb_h - cch) : 0;
      switch (LOWORD(wparam)) {
        case SB_LINEUP:
          self->scroll_y_ -= 20;
          break;
        case SB_LINEDOWN:
          self->scroll_y_ += 20;
          break;
        case SB_PAGEUP:
          self->scroll_y_ -= cch / 2;
          break;
        case SB_PAGEDOWN:
          self->scroll_y_ += cch / 2;
          break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
          self->scroll_y_ = static_cast<int>(HIWORD(wparam));
          break;
        default:
          break;
      }
      if (self->scroll_y_ < 0) {
        self->scroll_y_ = 0;
      }
      if (self->scroll_y_ > max_y) {
        self->scroll_y_ = max_y;
      }
      self->update_scrollbars();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (!self->fit_to_window_ && self->client_.connected() && !self->reconnecting_) {
        const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int keys = GET_KEYSTATE_WPARAM(wparam);
        const int line = (delta / WHEEL_DELTA) * 40;
        if (keys & MK_CONTROL) {
          self->scroll_x_ -= line;
        } else {
          self->scroll_y_ -= line;
        }
        int fb_w = 0;
        int fb_h = 0;
        if (self->client_.framebuffer_size(&fb_w, &fb_h)) {
          RECT crc{};
          GetClientRect(hwnd, &crc);
          const int ccw = crc.right - crc.left;
          const int cch = crc.bottom - crc.top;
          const int max_x = (fb_w > ccw) ? (fb_w - ccw) : 0;
          const int max_y = (fb_h > cch) ? (fb_h - cch) : 0;
          if (self->scroll_x_ < 0) {
            self->scroll_x_ = 0;
          }
          if (self->scroll_x_ > max_x) {
            self->scroll_x_ = max_x;
          }
          if (self->scroll_y_ < 0) {
            self->scroll_y_ = 0;
          }
          if (self->scroll_y_ > max_y) {
            self->scroll_y_ = max_y;
          }
        }
        self->update_scrollbars();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_PAINT:
      self->paint();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        // View-only: hide the local cursor so it does not obscure the remote frame.
        if (self->view_only_) {
          SetCursor(nullptr);
        } else {
          // Always local OS cursor (VNC-style). Host blanks system cursors so the remote
          // soft-cursor channel is empty — hiding the local cursor on hover left none.
          SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      self->tracking_leave_ = false;
      self->client_.set_software_cursor_enabled(false);
      return 0;
    case WM_CLIPBOARDUPDATE:
      if (!self->view_only_ && !self->reconnecting_ && self->client_.connected()) {
        self->client_.notify_clipboard_changed();
      }
      return 0;
    case WM_MEDIA_AUDIT_ACTIVITY: {
      if (wparam == 1 && !self->audit_clipboard_sent_) {
        self->audit_clipboard_sent_ = true;
        self->audit_emit("flag", "ok", nullptr, /*flag_clipboard=*/true, /*flag_file=*/false);
      } else if (wparam == 2 || wparam == 3) {
        const int entries = static_cast<int>(lparam);
        const int add = entries > 0 ? entries : 1;
        if (wparam == 2) {
          ++self->audit_file_out_count_;
          self->audit_file_out_entries_ += add;
        } else {
          ++self->audit_file_in_count_;
          self->audit_file_in_entries_ += add;
        }
        auto pending = self->client_.take_pending_audit_files();
        constexpr size_t kMaxAuditFiles = 64;
        for (auto& it : pending) {
          if (self->audit_file_items_.size() >= kMaxAuditFiles) {
            break;
          }
          self->audit_file_items_.push_back(std::move(it));
        }
        self->audit_file_sent_ = true;
        self->audit_emit("flag", "ok", nullptr, /*flag_clipboard=*/false, /*flag_file=*/true);
      }
      return 0;
    }
    case WM_MEDIA_SESSION_ROLE:
      self->apply_host_session_role();
      return 0;
    case WM_MEDIA_FRAME_DIRTY: {
      const int fx = static_cast<int>(LOWORD(wparam));
      const int fy = static_cast<int>(HIWORD(wparam));
      const int fw = static_cast<int>(LOWORD(lparam));
      const int fh = static_cast<int>(HIWORD(lparam));
      int desk_w = 0;
      int desk_h = 0;
      if (!self->client_.framebuffer_size(&desk_w, &desk_h) || desk_w <= 0 || desk_h <= 0) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      // Full-frame update → full invalidate.
      if (fx == 0 && fy == 0 && fw >= desk_w && fh >= desk_h) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      RECT crc{};
      GetClientRect(hwnd, &crc);
      const int ccw = crc.right - crc.left;
      const int cch = crc.bottom - crc.top;

      if (self->fit_to_window_) {
        RECT dest{};
        if (!fit_rect(ccw, cch, desk_w, desk_h, &dest, !self->floating_)) {
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        const int dw = dest.right - dest.left;
        const int dh = dest.bottom - dest.top;
        if (dw <= 0 || dh <= 0) {
          return 0;
        }
        RECT inv{};
        inv.left = dest.left + (fx * dw) / desk_w;
        inv.top = dest.top + (fy * dh) / desk_h;
        inv.right = dest.left + ((fx + fw) * dw + desk_w - 1) / desk_w;
        inv.bottom = dest.top + ((fy + fh) * dh + desk_h - 1) / desk_h;
        InflateRect(&inv, 4, 4);
        RECT clipped{};
        if (IntersectRect(&clipped, &inv, &crc)) {
          InvalidateRect(hwnd, &clipped, FALSE);
        }
      } else {
        // 1:1: map framebuffer dirty rect directly to client coords, offset by scroll.
        const int img_x = (desk_w < ccw) ? ((ccw - desk_w) / 2) : 0;
        const int img_y = (desk_h < cch) ? ((cch - desk_h) / 2) : 0;
        int cx = img_x + fx - self->scroll_x_;
        int cy = img_y + fy - self->scroll_y_;
        int cw_rect = fw;
        int ch_rect = fh;
        // Clip to client area.
        if (cx < 0) {
          cw_rect += cx;
          cx = 0;
        }
        if (cy < 0) {
          ch_rect += cy;
          cy = 0;
        }
        if (cx + cw_rect > ccw) {
          cw_rect = ccw - cx;
        }
        if (cy + ch_rect > cch) {
          ch_rect = cch - cy;
        }
        if (cw_rect > 0 && ch_rect > 0) {
          RECT inv{cx, cy, cx + cw_rect, cy + ch_rect};
          InflateRect(&inv, 4, 4);
          RECT clipped{};
          if (IntersectRect(&clipped, &inv, &crc)) {
            InvalidateRect(hwnd, &clipped, FALSE);
          }
        }
      }
      return 0;
    }
    case WM_MEDIA_TRANSPORT_LOST:
      self->on_transport_lost();
      return 0;
    case WM_TIMER:
      if (wparam == kReconnectTimerId) {
        KillTimer(hwnd, kReconnectTimerId);
        self->try_reconnect();
        return 0;
      }
      break;
    case WM_CLOSE:
      self->user_closing_ = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      self->on_destroy();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: {
      if (self->view_only_ || self->reconnecting_ || !self->client_.connected()) {
        break;
      }
      if (msg == WM_MOUSEMOVE) {
        if (!self->tracking_leave_) {
          TRACKMOUSEEVENT tme{};
          tme.cbSize = sizeof(tme);
          tme.dwFlags = TME_LEAVE;
          tme.hwndTrack = hwnd;
          if (TrackMouseEvent(&tme)) {
            self->tracking_leave_ = true;
          }
        }
      }
      int mask = 0;
      if (wparam & MK_LBUTTON) {
        mask |= 1;
      }
      if (wparam & MK_MBUTTON) {
        mask |= 2;
      }
      if (wparam & MK_RBUTTON) {
        mask |= 4;
      }
      int fb_w = 0;
      int fb_h = 0;
      if (!self->client_.framebuffer_size(&fb_w, &fb_h)) {
        return 0;
      }
      const bool fit_ws = !self->floating_;
      int mx = 0;
      int my = 0;
      if (self->fit_to_window_) {
        if (!client_point_in_letterbox(hwnd, lparam, fb_w, fb_h, fit_ws)) {
          self->client_.set_software_cursor_enabled(false);
          return 0;
        }
        mx = map_mouse_x(hwnd, lparam, fb_w, fb_h, fit_ws);
        my = map_mouse_y(hwnd, lparam, fb_w, fb_h, fit_ws);
      } else {
        // 1:1: check if the cursor is inside the drawn framebuffer area.
        RECT crc{};
        GetClientRect(hwnd, &crc);
        const int ccw = crc.right - crc.left;
        const int cch = crc.bottom - crc.top;
        const int img_x = (fb_w < ccw) ? ((ccw - fb_w) / 2) : 0;
        const int img_y = (fb_h < cch) ? ((cch - fb_h) / 2) : 0;
        const int img_w = (fb_w < ccw) ? fb_w : ccw;
        const int img_h = (fb_h < cch) ? fb_h : cch;
        const int cx = GET_X_LPARAM(lparam);
        const int cy = GET_Y_LPARAM(lparam);
        if (cx < img_x || cx >= img_x + img_w || cy < img_y || cy >= img_y + img_h) {
          self->client_.set_software_cursor_enabled(false);
          return 0;
        }
        mx = (cx - img_x) + self->scroll_x_;
        my = (cy - img_y) + self->scroll_y_;
        if (mx < 0) {
          mx = 0;
        }
        if (mx >= fb_w) {
          mx = fb_w - 1;
        }
        if (my < 0) {
          my = 0;
        }
        if (my >= fb_h) {
          my = fb_h - 1;
        }
      }
      // Soft cursor off: Host blanks OS cursors; local WM_SETCURSOR arrow is the only pointer.
      self->client_.set_software_cursor_enabled(false);
      self->client_.send_pointer(mask, mx, my);
      self->last_ptr_mask_ = mask;
      return 0;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
      if (msg == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE && self->floating_) {
        SessionHost::set_keyboard_target(self);
      }
      if (msg == WM_KILLFOCUS ||
          (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        self->client_.release_modifiers();
        self->client_.set_software_cursor_enabled(false);
      }
      break;
    case WM_KEYDOWN:
      if (self->fullscreen_ && wparam == VK_ESCAPE) {
        self->toggle_fullscreen();
        return 0;
      }
      [[fallthrough]];
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (!g_kb_hook && !self->view_only_ && !self->reconnecting_) {
        self->client_.send_vk(static_cast<unsigned>(wparam),
                              msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

SessionHost::SessionHost() = default;

SessionHost::~SessionHost() {
  close();
}

void SessionHost::set_audit_session(const std::string& session_id, const std::string& agent_id,
                                    const std::string& agent_name_utf8,
                                    const std::string& agent_endpoint) {
  audit_session_id_ = session_id;
  audit_agent_id_ = agent_id;
  audit_agent_name_ = agent_name_utf8;
  audit_agent_endpoint_ = agent_endpoint;
  audit_clipboard_sent_ = false;
  audit_file_sent_ = false;
  audit_file_out_count_ = 0;
  audit_file_in_count_ = 0;
  audit_file_out_entries_ = 0;
  audit_file_in_entries_ = 0;
  audit_file_items_.clear();
  audit_opened_sent_ = false;
}

void SessionHost::audit_emit(const char* phase, const char* result, const char* disconnect_reason,
                             bool flag_clipboard, bool flag_file) {
  if (!audit_reporting_enabled() || audit_session_id_.empty() || !phase) {
    return;
  }
  AuditReport r;
  r.session_id = audit_session_id_;
  r.phase = phase;
  r.agent_id = audit_agent_id_;
  r.agent_name = audit_agent_name_;
  r.agent_endpoint = audit_agent_endpoint_;
  r.mode = view_only_ ? "view_only" : "control";
  if (result && result[0]) {
    r.result = result;
  }
  if (disconnect_reason && disconnect_reason[0]) {
    r.disconnect_reason = disconnect_reason;
  }
  if (flag_clipboard || audit_clipboard_sent_) {
    r.set_clipboard = true;
    r.used_clipboard = true;
  }
  if (flag_file || audit_file_sent_) {
    r.set_file = true;
    r.used_file_transfer = true;
    r.file_out_count = audit_file_out_count_;
    r.file_in_count = audit_file_in_count_;
    r.file_out_entries = audit_file_out_entries_;
    r.file_in_entries = audit_file_in_entries_;
    r.file_items.reserve(audit_file_items_.size());
    for (const auto& it : audit_file_items_) {
      AuditFileItem row;
      row.path = it.path;
      row.name = it.name;
      row.is_dir = it.is_dir;
      row.outbound = it.outbound;
      r.file_items.push_back(std::move(row));
    }
  }
  if (reconnect_attempt_ > 0) {
    r.reconnect_count = reconnect_attempt_;
  }
  r.partial = true;
  audit_report_async(r);
}

bool SessionHost::register_class(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SessionWndProc;
  wc.hInstance = instance;
  wc.hCursor = nullptr;
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kSessionClass;
  rd::rd_apply_wndclass_icons(&wc, instance);
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  return true;
}

bool SessionHost::open(HINSTANCE instance, HWND parent_or_null, const std::wstring& title,
                       const ConnectDefaults& connect, ClosedFn on_closed, bool auto_reconnect) {
  if (hwnd_) {
    return false;
  }
  title_ = title;
  connect_ = connect;
  on_closed_ = std::move(on_closed);
  auto_reconnect_ = auto_reconnect;
  user_closing_ = false;
  reconnecting_ = false;
  reconnect_gave_up_ = false;
  reconnect_attempt_ = 0;
  status_override_.clear();
  floating_ = (parent_or_null == nullptr);

  DWORD style = floating_ ? WS_OVERLAPPEDWINDOW : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
  DWORD ex = 0;
  hwnd_ = CreateWindowExW(ex, kSessionClass, title_.c_str(), style, 0, 0,
                          floating_ ? 1024 : 100, floating_ ? 768 : 100, parent_or_null, nullptr,
                          instance, this);
  if (!hwnd_) {
    return false;
  }
  if (floating_) {
    rd::rd_set_window_icons(hwnd_, instance);
  }
  AddClipboardFormatListener(hwnd_);

  road_desk::media::MediaClientConfig cfg = make_client_config();
  if (cfg.require_tls && !cfg.tls_insecure && cfg.tls_fingerprint_sha256.empty()) {
    audit_emit("failed", "tls_fail", nullptr);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  audit_emit("attempt", "unknown", nullptr);
  if (!client_.start(cfg)) {
    using road_desk::media::MediaClientFail;
    const MediaClientFail fail = client_.last_fail();
    const char* result = "connect_fail";
    if (fail == MediaClientFail::Auth) {
      result = "auth_fail";
    } else if (fail == MediaClientFail::Config) {
      result = "tls_fail";
    }
    audit_emit("failed", result, nullptr);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  apply_host_session_role();
  started_ = true;
  audit_opened_sent_ = true;
  audit_emit("opened", "ok", nullptr);
  update_scrollbars();
  if (floating_) {
    ShowWindow(hwnd_, SW_SHOW);
  }
  return true;
}

void SessionHost::close() {
  user_closing_ = true;
  if (hwnd_) {
    KillTimer(hwnd_, kReconnectTimerId);
    DestroyWindow(hwnd_);
    // on_destroy clears hwnd_
  } else if (started_) {
    client_.stop();
    started_ = false;
  }
}

bool SessionHost::connected() const {
  return started_ && client_.connected() && !reconnecting_;
}

std::wstring SessionHost::status_text() const {
  if (!status_override_.empty()) {
    return status_override_;
  }
  if (reconnecting_) {
    wchar_t buf[64] = {};
    _snwprintf_s(buf, _TRUNCATE, L"正在重连… (%d)", reconnect_attempt_ + 1);
    return buf;
  }
  return {};
}

  road_desk::media::MediaClientConfig SessionHost::make_client_config() const {
  road_desk::media::MediaClientConfig cfg;
  apply_connect_defaults_to(&cfg, connect_);
  cfg.notify_hwnd = hwnd_;
  cfg.resize_msg = WM_MEDIA_RESIZE;
  cfg.disconnect_msg = WM_MEDIA_TRANSPORT_LOST;
  cfg.audit_activity_msg = WM_MEDIA_AUDIT_ACTIVITY;
  cfg.session_role_msg = WM_MEDIA_SESSION_ROLE;
  cfg.frame_dirty_msg = WM_MEDIA_FRAME_DIRTY;
  cfg.audit_session_id = audit_session_id_;
  return cfg;
}

int SessionHost::reconnect_delay_ms() const {
  int shift = reconnect_attempt_;
  if (shift > 5) {
    shift = 5;
  }
  int ms = 1000 << shift;
  if (ms > 30000) {
    ms = 30000;
  }
  return ms;
}

void SessionHost::notify_chrome_changed() {
  if (!hwnd_) {
    return;
  }
  HWND root = GetAncestor(hwnd_, GA_ROOT);
  if (root) {
    PostMessageW(root, WM_SESSION_META, 0, reinterpret_cast<LPARAM>(this));
  }
}

void SessionHost::schedule_reconnect() {
  if (!hwnd_ || user_closing_ || !auto_reconnect_ || reconnect_gave_up_) {
    return;
  }
  const int delay = reconnect_delay_ms();
  road_desk::media::media_logf("viewer", "reconnect schedule attempt=%d delay_ms=%d host=%s",
                               reconnect_attempt_ + 1, delay, connect_.host_port.c_str());
  status_override_.clear();
  reconnecting_ = true;
  KillTimer(hwnd_, kReconnectTimerId);
  SetTimer(hwnd_, kReconnectTimerId, static_cast<UINT>(delay), nullptr);
  notify_chrome_changed();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SessionHost::stop_reconnect(const wchar_t* status) {
  if (hwnd_) {
    KillTimer(hwnd_, kReconnectTimerId);
  }
  reconnecting_ = false;
  reconnect_gave_up_ = true;
  if (status && status[0]) {
    status_override_ = status;
  }
  road_desk::media::media_logf("viewer", "reconnect stopped host=%s", connect_.host_port.c_str());
  using road_desk::media::MediaClientFail;
  const MediaClientFail fail = client_.last_fail();
  const char* result = "connect_fail";
  if (fail == MediaClientFail::Auth) {
    result = "auth_fail";
  } else if (fail == MediaClientFail::Config) {
    result = "tls_fail";
  }
  audit_emit("failed", result, "transport_lost");
  notify_chrome_changed();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SessionHost::on_transport_lost() {
  if (user_closing_ || !hwnd_) {
    return;
  }
  client_.release_modifiers();
  client_.stop();
  started_ = false;

  if (!auto_reconnect_ || reconnect_gave_up_) {
    road_desk::media::media_logf("viewer", "transport lost (no reconnect) host=%s",
                                 connect_.host_port.c_str());
    if (!user_closing_) {
      audit_emit("closed", "ok", "transport_lost");
    }
    DestroyWindow(hwnd_);
    return;
  }

  road_desk::media::media_logf("viewer", "transport lost → reconnect host=%s",
                               connect_.host_port.c_str());
  schedule_reconnect();
}

void SessionHost::try_reconnect() {
  if (user_closing_ || !hwnd_ || !auto_reconnect_ || reconnect_gave_up_) {
    return;
  }
  reconnecting_ = true;
  status_override_.clear();
  ++reconnect_attempt_;
  road_desk::media::media_logf("viewer", "reconnect try n=%d host=%s", reconnect_attempt_,
                               connect_.host_port.c_str());
  notify_chrome_changed();

  client_.stop();
  road_desk::media::MediaClientConfig cfg = make_client_config();
  if (!client_.start(cfg)) {
    using road_desk::media::MediaClientFail;
    const MediaClientFail fail = client_.last_fail();
    road_desk::media::media_logf("viewer", "reconnect fail n=%d kind=%u host=%s",
                                 reconnect_attempt_, static_cast<unsigned>(fail),
                                 connect_.host_port.c_str());
    if (fail == MediaClientFail::Auth || fail == MediaClientFail::Config) {
      const wchar_t* msg = (fail == MediaClientFail::Auth) ? L"重连已停止：认证失败"
                                                           : L"重连已停止：配置无效";
      stop_reconnect(msg);
      MessageBoxW(hwnd_, msg, L"Road Desk", MB_OK | MB_ICONWARNING);
      return;
    }
    schedule_reconnect();
    return;
  }

  apply_host_session_role();
  started_ = true;
  reconnecting_ = false;
  update_scrollbars();
  const int prior_attempts = reconnect_attempt_;
  reconnect_attempt_ = 0;
  status_override_.clear();
  road_desk::media::media_logf("viewer", "reconnect ok host=%s", connect_.host_port.c_str());
  if (prior_attempts > 0) {
    AuditReport r;
    r.session_id = audit_session_id_;
    r.phase = "flag";
    r.agent_id = audit_agent_id_;
    r.agent_name = audit_agent_name_;
    r.agent_endpoint = audit_agent_endpoint_;
    r.mode = view_only_ ? "view_only" : "control";
    r.result = "ok";
    r.reconnect_count = prior_attempts;
    r.partial = true;
    audit_report_async(r);
  }
  notify_chrome_changed();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SessionHost::detach_to_floating(HINSTANCE instance) {
  if (!hwnd_ || floating_) {
    return floating_;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  SetParent(hwnd_, nullptr);
  const LONG_PTR style =
      (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(WS_CHILD)) | WS_OVERLAPPEDWINDOW;
  SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
  SetWindowPos(hwnd_, nullptr, rc.left + 40, rc.top + 40, rc.right - rc.left,
               rc.bottom - rc.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  SetWindowTextW(hwnd_, title_.c_str());
  floating_ = true;
  client_.set_notify_hwnd(hwnd_);
  (void)instance;
  return true;
}

bool SessionHost::attach_to_parent(HWND parent) {
  if (!hwnd_ || !parent || !floating_) {
    return false;
  }
  SetParent(hwnd_, parent);
  const LONG_PTR style =
      (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW)) | WS_CHILD | WS_VISIBLE;
  SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
  floating_ = false;
  client_.set_notify_hwnd(hwnd_);
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
  return true;
}

bool SessionHost::wants_keyboard() const {
  if (view_only_ || reconnecting_ || !hwnd_ || g_kb_target != this || !client_.connected()) {
    return false;
  }
  HWND fg = GetForegroundWindow();
  if (floating_) {
    return fg == hwnd_;
  }
  // Docked: console (root owner) must be foreground.
  return fg == GetAncestor(hwnd_, GA_ROOT);
}

bool SessionHost::send_vk(unsigned vk, bool down) {
  return client_.send_vk(vk, down);
}

void SessionHost::release_modifiers() {
  client_.release_modifiers();
}

void SessionHost::set_view_only(bool on) {
  if (host_forced_view_only_ && !on) {
    // Host is source of truth: cannot take control while another Viewer holds it.
    return;
  }
  if (view_only_ == on) {
    return;
  }
  view_only_ = on;
  if (on) {
    client_.release_modifiers();
  }
  if (audit_opened_sent_) {
    audit_emit("flag", "ok", nullptr);
  }
  notify_chrome_changed();
}

void SessionHost::apply_host_session_role() {
  const bool forced = client_.host_forces_view_only();
  const bool was_view_only = view_only_;
  if (forced) {
    host_forced_view_only_ = true;
    if (!view_only_) {
      view_only_ = true;
      client_.release_modifiers();
    }
  } else if (host_forced_view_only_) {
    // Was Host-forced watch; Host granted control (promote or reconnect) — lift freeze.
    host_forced_view_only_ = false;
    view_only_ = false;
  } else {
    host_forced_view_only_ = false;
  }
  if (audit_opened_sent_ && was_view_only != view_only_) {
    audit_emit("flag", "ok", nullptr);
  }
  notify_chrome_changed();
}

bool SessionHost::toggle_fullscreen() {
  if (!hwnd_) {
    return false;
  }
  if (fullscreen_) {
    SetWindowLongPtrW(hwnd_, GWL_STYLE, fullscreen_restore_style_);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, fullscreen_restore_rect_.left,
                 fullscreen_restore_rect_.top,
                 fullscreen_restore_rect_.right - fullscreen_restore_rect_.left,
                 fullscreen_restore_rect_.bottom - fullscreen_restore_rect_.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    fullscreen_ = false;

    // Docked → fullscreen detaches first; Esc must put the session back in the console.
    if (fullscreen_dock_parent_ && IsWindow(fullscreen_dock_parent_)) {
      HWND parent = fullscreen_dock_parent_;
      fullscreen_dock_parent_ = nullptr;
      if (attach_to_parent(parent)) {
        RECT prc{};
        GetClientRect(parent, &prc);
        SetWindowPos(hwnd_, nullptr, 0, 0, prc.right - prc.left, prc.bottom - prc.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      }
    } else {
      fullscreen_dock_parent_ = nullptr;
    }
    notify_chrome_changed();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
  }

  // Docked sessions detach first so the session can cover the whole monitor.
  fullscreen_dock_parent_ = nullptr;
  if (!floating_) {
    HWND parent = GetParent(hwnd_);
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    if (!parent || !detach_to_floating(inst)) {
      return false;
    }
    fullscreen_dock_parent_ = parent;
  }

  GetWindowRect(hwnd_, &fullscreen_restore_rect_);
  fullscreen_restore_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
  HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) {
    // Roll back detach if we failed after tearing out of the console.
    if (fullscreen_dock_parent_) {
      HWND parent = fullscreen_dock_parent_;
      fullscreen_dock_parent_ = nullptr;
      attach_to_parent(parent);
      notify_chrome_changed();
    }
    return false;
  }
  const LONG_PTR style =
      (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE;
  SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
  SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
               mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  fullscreen_ = true;
  SetFocus(hwnd_);
  notify_chrome_changed();
  InvalidateRect(hwnd_, nullptr, TRUE);
  return true;
}

bool SessionHost::save_screenshot(const std::wstring& path) {
  const uint32_t epoch = client_.framebuffer_epoch();
  if (epoch != desk_epoch_ || desk_bgra_.empty()) {
    int w = 0;
    int h = 0;
    if (!client_.copy_desktop_bgra(desk_bgra_, w, h)) {
      return false;
    }
    desk_w_ = w;
    desk_h_ = h;
    desk_epoch_ = epoch;
  }
  if (desk_w_ <= 0 || desk_h_ <= 0 || desk_bgra_.empty()) {
    return false;
  }
  const size_t need =
      static_cast<size_t>(desk_w_) * static_cast<size_t>(desk_h_) * 4u;
  if (desk_bgra_.size() < need) {
    return false;
  }
  std::vector<uint8_t> bgra = desk_bgra_;
  client_.composite_software_cursor(bgra, desk_w_, desk_h_);

  // Force opaque alpha — remote FB often leaves A=0; PNG would look empty.
  for (size_t i = 3; i < bgra.size(); i += 4) {
    bgra[i] = 255;
  }

  gdip::GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  if (gdip::GdiplusStartup(&token, &gsi, nullptr) != gdip::Ok) {
    return false;
  }
  // Bitmap must be destroyed before GdiplusShutdown or the dtor AVs.
  gdip::Status st = gdip::GenericError;
  {
    gdip::Bitmap bmp(desk_w_, desk_h_, desk_w_ * 4, PixelFormat32bppARGB, bgra.data());
    // PNG encoder CLSID {557cf406-1a04-11d3-9a73-0000f81ef32e}.
    CLSID png_clsid = {0x557cf406, 0x1a04, 0x11d3,
                       {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    st = bmp.Save(path.c_str(), &png_clsid, nullptr);
  }
  gdip::GdiplusShutdown(token);
  return st == gdip::Ok;
}

SessionHost* SessionHost::from_hwnd(HWND hwnd) {
  if (!hwnd) {
    return nullptr;
  }
  return reinterpret_cast<SessionHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void SessionHost::set_fit_to_window(bool on) {
  if (fit_to_window_ == on) {
    return;
  }
  fit_to_window_ = on;
  // Mode switch always forces a full repaint — the pixel data is the same
  // framebuffer, but the mapping rules changed (scale vs scroll-offset).
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    update_scrollbars();
  }
}

void SessionHost::update_scrollbars() {
  if (!hwnd_) {
    return;
  }
  int fb_w = 0;
  int fb_h = 0;
  if (!client_.framebuffer_size(&fb_w, &fb_h)) {
    fb_w = 0;
    fb_h = 0;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  const bool need_scroll = !fit_to_window_ && fb_w > 0 && fb_h > 0 && (fb_w > cw || fb_h > ch);
  const LONG_PTR base_style = floating_ ? WS_OVERLAPPEDWINDOW : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
  const LONG_PTR scroll_style = need_scroll ? (WS_HSCROLL | WS_VSCROLL) : 0;
  const LONG_PTR cur = GetWindowLongPtrW(hwnd_, GWL_STYLE);
  const LONG_PTR desired = base_style | scroll_style;
  if ((cur & (WS_HSCROLL | WS_VSCROLL)) != scroll_style) {
    SetWindowLongPtrW(hwnd_, GWL_STYLE, desired);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }

  if (need_scroll) {
    const int max_x = (fb_w > cw) ? (fb_w - cw) : 0;
    const int max_y = (fb_h > ch) ? (fb_h - ch) : 0;
    if (scroll_x_ < 0) {
      scroll_x_ = 0;
    }
    if (scroll_x_ > max_x) {
      scroll_x_ = max_x;
    }
    if (scroll_y_ < 0) {
      scroll_y_ = 0;
    }
    if (scroll_y_ > max_y) {
      scroll_y_ = max_y;
    }

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = fb_w - 1;
    si.nPage = static_cast<UINT>(cw);
    si.nPos = scroll_x_;
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);

    si.nMax = fb_h - 1;
    si.nPage = static_cast<UINT>(ch);
    si.nPos = scroll_y_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
  } else {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 100;
    si.nPage = 101;
    si.nPos = 0;
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    scroll_x_ = 0;
    scroll_y_ = 0;
  }
}

void SessionHost::install_keyboard_hook(HINSTANCE instance) {
  if (!g_kb_hook) {
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard, instance, 0);
  }
}

void SessionHost::uninstall_keyboard_hook() {
  if (g_kb_hook) {
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
  }
  g_kb_target = nullptr;
}

void SessionHost::set_keyboard_target(SessionHost* host) {
  if (g_kb_target && g_kb_target != host) {
    g_kb_target->release_modifiers();
  }
  g_kb_target = host;
}

void SessionHost::release_backbuffer() {
  if (back_dc_ && back_old_) {
    SelectObject(back_dc_, back_old_);
    back_old_ = nullptr;
  }
  if (back_bmp_) {
    DeleteObject(back_bmp_);
    back_bmp_ = nullptr;
  }
  if (back_dc_) {
    DeleteDC(back_dc_);
    back_dc_ = nullptr;
  }
  back_w_ = 0;
  back_h_ = 0;
}

bool SessionHost::ensure_backbuffer(HDC hdc, int cw, int ch) {
  if (cw <= 0 || ch <= 0) {
    return false;
  }
  if (back_dc_ && back_bmp_ && back_w_ == cw && back_h_ == ch) {
    return true;
  }
  release_backbuffer();
  back_dc_ = CreateCompatibleDC(hdc);
  back_bmp_ = CreateCompatibleBitmap(hdc, cw, ch);
  if (!back_dc_ || !back_bmp_) {
    release_backbuffer();
    return false;
  }
  back_old_ = SelectObject(back_dc_, back_bmp_);
  back_w_ = cw;
  back_h_ = ch;
  return true;
}

void SessionHost::paint() {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd_, &ps);

  const uint32_t epoch = client_.framebuffer_epoch();
  if (epoch != desk_epoch_ || desk_bgra_.empty()) {
    int w = 0;
    int h = 0;
    if (client_.copy_desktop_bgra(desk_bgra_, w, h)) {
      desk_w_ = w;
      desk_h_ = h;
      desk_epoch_ = epoch;
    }
  }

  std::vector<uint8_t> bgra = desk_bgra_;
  const int w = desk_w_;
  const int h = desk_h_;
  if (w > 0 && h > 0 && !bgra.empty()) {
    client_.composite_software_cursor(bgra, w, h);
  }

  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0 || !ensure_backbuffer(hdc, cw, ch)) {
    EndPaint(hwnd_, &ps);
    return;
  }

  const bool have_frame = w > 0 && h > 0 && bgra.size() >= static_cast<size_t>(w) * h * 4;

  if (have_frame && fit_to_window_) {
    // --- fit-to-window path (existing behaviour) ---
    RECT dest{};
    if (fit_rect(cw, ch, w, h, &dest, !floating_)) {
      const int dw = dest.right - dest.left;
      const int dh = dest.bottom - dest.top;
      RECT update{};
      if (IntersectRect(&update, &ps.rcPaint, &rc)) {
        auto fill_black = [&](int l, int t, int r, int b) {
          if (r <= l || b <= t) {
            return;
          }
          RECT bar{l, t, r, b};
          RECT hit{};
          if (IntersectRect(&hit, &bar, &update)) {
            FillRect(back_dc_, &hit, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
          }
        };
        fill_black(0, 0, cw, dest.top);
        fill_black(0, dest.bottom, cw, ch);
        fill_black(0, dest.top, dest.left, dest.bottom);
        fill_black(dest.right, dest.top, cw, dest.bottom);

        RECT paint_dest{};
        if (IntersectRect(&paint_dest, &update, &dest) && dw > 0 && dh > 0) {
          BITMAPINFO bmi{};
          bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
          bmi.bmiHeader.biWidth = w;
          bmi.bmiHeader.biHeight = -h;
          bmi.bmiHeader.biPlanes = 1;
          bmi.bmiHeader.biBitCount = 32;
          bmi.bmiHeader.biCompression = BI_RGB;
          SetStretchBltMode(back_dc_, COLORONCOLOR);
          SetBrushOrgEx(back_dc_, 0, 0, nullptr);
          StretchDIBits(back_dc_, dest.left, dest.top, dw, dh, 0, 0, w, h, bgra.data(), &bmi,
                        DIB_RGB_COLORS, SRCCOPY);
        }
      }
    }
  } else if (have_frame) {
    // --- 1:1 path (default) — pixel-perfect, no scaling, scrollbars when larger ---
    const int draw_w = (w < cw) ? w : cw;
    const int draw_h = (h < ch) ? h : ch;
    // Centre when framebuffer is smaller than client; anchor at (0,0) when larger.
    const int dest_x = (w < cw) ? ((cw - w) / 2) : 0;
    const int dest_y = (h < ch) ? ((ch - h) / 2) : 0;

    RECT update{};
    if (!IntersectRect(&update, &ps.rcPaint, &rc)) {
      EndPaint(hwnd_, &ps);
      return;
    }

    auto fill_black = [&](int l, int t, int r, int b) {
      if (r <= l || b <= t) {
        return;
      }
      RECT bar{l, t, r, b};
      RECT hit{};
      if (IntersectRect(&hit, &bar, &update)) {
        FillRect(back_dc_, &hit, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      }
    };
    fill_black(0, 0, cw, dest_y);
    fill_black(0, dest_y + draw_h, cw, ch);
    fill_black(0, dest_y, dest_x, dest_y + draw_h);
    fill_black(dest_x + draw_w, dest_y, cw, dest_y + draw_h);

    // Blit only the visible portion of the framebuffer 1:1 (no Stretch).
    const int src_x = scroll_x_;
    const int src_y = scroll_y_;
    RECT paint_dest{};
    RECT draw_rc{dest_x, dest_y, dest_x + draw_w, dest_y + draw_h};
    if (IntersectRect(&paint_dest, &update, &draw_rc) && draw_w > 0 && draw_h > 0) {
      BITMAPINFO bmi{};
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = w;
      bmi.bmiHeader.biHeight = -h;
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      SetStretchBltMode(back_dc_, COLORONCOLOR);
      SetBrushOrgEx(back_dc_, 0, 0, nullptr);
      StretchDIBits(back_dc_, dest_x, dest_y, draw_w, draw_h, src_x, src_y, draw_w, draw_h,
                    bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
  } else {
    FillRect(back_dc_, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    // Design Frame B: monitor icon + 「正在连接被控端…」+ sessionHint + body font.
    const int dpi = rd::rd_dpi_screen();
    HFONT font = rd::rd_create_font(dpi, rd::kFontBodyPt);
    HGDIOBJ old_font = font ? SelectObject(back_dc_, font) : nullptr;
    SetBkMode(back_dc_, TRANSPARENT);
    SetTextColor(back_dc_, rd::kColorTextSessionHint);

    const wchar_t* msg = L"正在连接被控端…";
    std::wstring owned_msg;
    if (reconnecting_ || reconnect_gave_up_) {
      owned_msg = status_text();
      if (!owned_msg.empty()) {
        msg = owned_msg.c_str();
      }
    }
    SIZE tsz{};
    GetTextExtentPoint32W(back_dc_, msg, static_cast<int>(wcslen(msg)), &tsz);
    const int icon = rd::rd_dip(22, dpi);
    const int gap = rd::rd_dip(rd::kSpace2 + 2, dpi);
    const int block_h = icon + gap + tsz.cy;
    const int top = rc.top + (ch - block_h) / 2;
    const int cx = rc.left + cw / 2;

    HPEN pen = CreatePen(PS_SOLID, 2, rd::kColorTextSessionHint);
    HGDIOBJ old_pen = SelectObject(back_dc_, pen);
    HGDIOBJ old_br = SelectObject(back_dc_, GetStockObject(NULL_BRUSH));
    const int ml = cx - icon / 2;
    const int mt = top;
    RoundRect(back_dc_, ml, mt, ml + icon, mt + icon * 3 / 4, 2, 2);
    MoveToEx(back_dc_, cx, mt + icon * 3 / 4, nullptr);
    LineTo(back_dc_, cx, mt + icon);
    MoveToEx(back_dc_, cx - icon / 4, mt + icon, nullptr);
    LineTo(back_dc_, cx + icon / 4, mt + icon);
    SelectObject(back_dc_, old_pen);
    SelectObject(back_dc_, old_br);
    DeleteObject(pen);

    RECT text_rc{rc.left, top + icon + gap, rc.right, top + icon + gap + tsz.cy};
    DrawTextW(back_dc_, msg, -1, &text_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
    if (old_font) {
      SelectObject(back_dc_, old_font);
    }
    if (font) {
      DeleteObject(font);
    }
  }

  // Blit only the update region (not the entire client) to avoid whole-window flash.
  BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
         ps.rcPaint.bottom - ps.rcPaint.top, back_dc_, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

  if (reconnecting_ || reconnect_gave_up_) {
    std::wstring banner = status_text();
    if (banner.empty()) {
      banner = L"正在重连…";
    }
    RECT band{0, ch - rd::kEditHDip, cw, ch};
    if (ch > 40) {
      const int dpi = rd::rd_dpi_screen();
      const int band_h = rd::rd_dip(rd::kEditHDip, dpi);
      band = {0, ch - band_h, cw, ch};
      HBRUSH br = CreateSolidBrush(rd::kColorBrandBg);
      FillRect(hdc, &band, br);
      DeleteObject(br);
      HFONT font = rd::rd_create_font(dpi, rd::kFontCaptionPt);
      HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, rd::kColorSurfaceChrome);
      DrawTextW(hdc, banner.c_str(), -1, &band, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old) {
        SelectObject(hdc, old);
      }
      if (font) {
        DeleteObject(font);
      }
    }
  }

  EndPaint(hwnd_, &ps);
}

void SessionHost::on_destroy() {
  if (hwnd_) {
    KillTimer(hwnd_, kReconnectTimerId);
  }
  if (user_closing_ && audit_opened_sent_) {
    audit_emit("closed", "ok", "user_close");
  } else if (reconnect_gave_up_ && audit_opened_sent_) {
    // failed already emitted in stop_reconnect; still close the bill
    audit_emit("closed", nullptr, "transport_lost");
  }
  RemoveClipboardFormatListener(hwnd_);
  release_backbuffer();
  if (started_) {
    client_.stop();
    started_ = false;
  }
  reconnecting_ = false;
  if (g_kb_target == this) {
    g_kb_target = nullptr;
  }
  hwnd_ = nullptr;
  floating_ = false;
  // Never delete `this` synchronously from WndProc — notify owner on next pump.
  if (on_closed_) {
    ClosedFn fn = std::move(on_closed_);
    on_closed_ = nullptr;
    fn(this);
  }
}

}  // namespace road_desk::viewer
