#include "lean_os/ozone/leanos_window.h"

#include <input.h>
#include <window_manager_client.h>

#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/display/types/display_constants.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/types/event_type.h"

namespace ui {

namespace {

// The compositor's title field is sixteen bytes and it is also what the
// taskbar maps to an icon, so the title is the one the desktop shell already
// knows rather than whatever content_shell would set.
constexpr char kWindowTitle[] = "Browser";

// The keyboard driver delivers a character, not a scancode - the arrows and
// the function keys are the values in system_api/include/input.h, and a
// modifier arrives beside the character rather than changing it, so ctrl+a is
// 'a' with KEYBOARD_MOD_CTRL. This turns that into what a ui::KeyEvent is
// made of: a Windows-style KeyboardCode, the physical DomCode a US layout
// would have produced it from, and the DomKey meaning ozone's layout engine
// would assign - through ui's own US-layout tables, so a key here means what
// it means on every other platform.
struct TranslatedKey {
  KeyboardCode key_code = VKEY_UNKNOWN;
  DomCode dom_code = DomCode::NONE;
  DomKey dom_key = DomKey::NONE;
  int flags = EF_NONE;
};

bool KeyFromCharacter(char ch, uint8_t mods, TranslatedKey* out) {
  const unsigned char c = static_cast<unsigned char>(ch);
  int flags = EF_NONE;
  if (mods & KEYBOARD_MOD_CTRL)
    flags |= EF_CONTROL_DOWN;
  if (mods & KEYBOARD_MOD_ALT)
    flags |= EF_ALT_DOWN;
  if (mods & KEYBOARD_MOD_SHIFT)
    flags |= EF_SHIFT_DOWN;

  KeyboardCode key_code = VKEY_UNKNOWN;
  bool shifted = false;
  switch (c) {
    case KEYBOARD_KEY_UP:    key_code = VKEY_UP; break;
    case KEYBOARD_KEY_DOWN:  key_code = VKEY_DOWN; break;
    case KEYBOARD_KEY_LEFT:  key_code = VKEY_LEFT; break;
    case KEYBOARD_KEY_RIGHT: key_code = VKEY_RIGHT; break;
    case '\b':               key_code = VKEY_BACK; break;
    case '\t':               key_code = VKEY_TAB; break;
    case '\n':
    case '\r':               key_code = VKEY_RETURN; break;
    case 27:                 key_code = VKEY_ESCAPE; break;
    case ' ':                key_code = VKEY_SPACE; break;
    case ',': key_code = VKEY_OEM_COMMA; break;
    case '<': key_code = VKEY_OEM_COMMA; shifted = true; break;
    case '.': key_code = VKEY_OEM_PERIOD; break;
    case '>': key_code = VKEY_OEM_PERIOD; shifted = true; break;
    case '/': key_code = VKEY_OEM_2; break;
    case '?': key_code = VKEY_OEM_2; shifted = true; break;
    case ';': key_code = VKEY_OEM_1; break;
    case ':': key_code = VKEY_OEM_1; shifted = true; break;
    case '\'': key_code = VKEY_OEM_7; break;
    case '"': key_code = VKEY_OEM_7; shifted = true; break;
    case '[': key_code = VKEY_OEM_4; break;
    case '{': key_code = VKEY_OEM_4; shifted = true; break;
    case ']': key_code = VKEY_OEM_6; break;
    case '}': key_code = VKEY_OEM_6; shifted = true; break;
    case '\\': key_code = VKEY_OEM_5; break;
    case '|': key_code = VKEY_OEM_5; shifted = true; break;
    case '`': key_code = VKEY_OEM_3; break;
    case '~': key_code = VKEY_OEM_3; shifted = true; break;
    case '-': key_code = VKEY_OEM_MINUS; break;
    case '_': key_code = VKEY_OEM_MINUS; shifted = true; break;
    case '=': key_code = VKEY_OEM_PLUS; break;
    case '+': key_code = VKEY_OEM_PLUS; shifted = true; break;
    case ')': key_code = VKEY_0; shifted = true; break;
    case '!': key_code = VKEY_1; shifted = true; break;
    case '@': key_code = VKEY_2; shifted = true; break;
    case '#': key_code = VKEY_3; shifted = true; break;
    case '$': key_code = VKEY_4; shifted = true; break;
    case '%': key_code = VKEY_5; shifted = true; break;
    case '^': key_code = VKEY_6; shifted = true; break;
    case '&': key_code = VKEY_7; shifted = true; break;
    case '*': key_code = VKEY_8; shifted = true; break;
    case '(': key_code = VKEY_9; shifted = true; break;
    default:
      if (c >= KEYBOARD_KEY_F1 && c <= KEYBOARD_KEY_F12) {
        key_code = static_cast<KeyboardCode>(VKEY_F1 + (c - KEYBOARD_KEY_F1));
      } else if (c >= '0' && c <= '9') {
        key_code = static_cast<KeyboardCode>(VKEY_0 + (c - '0'));
      } else if (c >= 'a' && c <= 'z') {
        key_code = static_cast<KeyboardCode>(VKEY_A + (c - 'a'));
      } else if (c >= 'A' && c <= 'Z') {
        key_code = static_cast<KeyboardCode>(VKEY_A + (c - 'A'));
        shifted = true;
      } else {
        return false;
      }
      break;
  }
  if (shifted)
    flags |= EF_SHIFT_DOWN;

  out->key_code = key_code;
  out->flags = flags;
  out->dom_code = UsLayoutKeyboardCodeToDomCode(key_code);
  KeyboardCode located = key_code;
  if (!DomCodeToUsLayoutDomKey(out->dom_code, flags, &out->dom_key, &located)) {
    out->dom_key = DomKey::NONE;
  }
  return true;
}

int ButtonFlags(uint8_t buttons) {
  int flags = EF_NONE;
  if (buttons & 1u)
    flags |= EF_LEFT_MOUSE_BUTTON;
  if (buttons & 2u)
    flags |= EF_RIGHT_MOUSE_BUTTON;
  if (buttons & 4u)
    flags |= EF_MIDDLE_MOUSE_BUTTON;
  return flags;
}

int ModifierFlags(uint8_t mods) {
  int flags = EF_NONE;
  if (mods & KEYBOARD_MOD_CTRL)
    flags |= EF_CONTROL_DOWN;
  if (mods & KEYBOARD_MOD_ALT)
    flags |= EF_ALT_DOWN;
  if (mods & KEYBOARD_MOD_SHIFT)
    flags |= EF_SHIFT_DOWN;
  return flags;
}

}  // namespace

LeanOsWindow::LeanOsWindow(PlatformWindowDelegate* delegate,
                           LeanOsWindowManager* manager,
                           const PlatformWindowInitProperties& properties)
    : delegate_(delegate), manager_(manager), bounds_(properties.bounds) {
  widget_ = manager_->AddWindow(this);

  // Two shapes of window connect to the compositor: a top-level window, and
  // a POPUP - a menu, a tooltip, a <select> list, an autofill dropdown. A
  // popup is a window the client positions, above every ordinary window, with
  // no title bar of its own and gone with its parent; the compositor learned
  // to place one in M174. Chromium positions a popup in "screen" coordinates,
  // and because every LeanOsWindow reports its origin as (0,0) the number it
  // computes is already relative to the parent's client area, which is what
  // window_manager_connect_popup wants.
  //
  // Everything else - a drag image, a bubble with no parent - stays a
  // PlatformWindow that draws nowhere, because the compositor has no request
  // for a window with no parent to hang off.
  const bool is_popup = properties.type == PlatformWindowType::kPopup ||
                        properties.type == PlatformWindowType::kMenu ||
                        properties.type == PlatformWindowType::kTooltip ||
                        properties.type == PlatformWindowType::kBubble;

  uint32_t width = bounds_.width() > 0 ? bounds_.width() : 1;
  uint32_t height = bounds_.height() > 0 ? bounds_.height() : 1;
  auto connection = std::make_unique<window_manager_window_t>();
  int connected = -1;

  if (properties.type == PlatformWindowType::kWindow) {
    connected = window_manager_connect(width > 1 ? width : 800,
                                       height > 1 ? height : 600,
                                       kWindowTitle, connection.get());
  } else if (is_popup) {
    LeanOsWindow* parent = properties.parent_widget != gfx::kNullAcceleratedWidget
                               ? manager_->GetWindow(properties.parent_widget)
                               : nullptr;
    if (parent && parent->connection_) {
      is_popup_ = true;
      connected = window_manager_connect_popup(parent->connection_->window_id,
                                               bounds_.x(), bounds_.y(),
                                               width, height, connection.get());
    }
  }

  if (connected == 0) {
    connection_ = std::move(connection);
    bounds_ = gfx::Rect(bounds_.x(), bounds_.y(), connection_->graphics.width,
                        connection_->graphics.height);
    buffer_ = base::MakeRefCounted<LeanOsWindowBuffer>(
        connection_->window_id, connection_->graphics.pixels,
        gfx::Size(connection_->graphics.width, connection_->graphics.height));
    manager_->SetBuffer(widget_, buffer_);
    event_watcher_ = base::FileDescriptorWatcher::WatchReadable(
        connection_->evt_file_descriptor,
        base::BindRepeating(&LeanOsWindow::OnEventPipeReadable,
                            base::Unretained(this)));
  } else if (properties.type == PlatformWindowType::kWindow) {
    LOG(ERROR) << "leanos: the compositor refused a " << width << "x" << height
               << " window - is the desktop running?";
  }

  VLOG(1) << "leanos: window " << widget_ << " created, type "
          << static_cast<int>(properties.type) << ", " << bounds_.ToString()
          << (connection_ ? ", connected" : ", not connected");
  delegate_->OnAcceleratedWidgetAvailable(widget_);
}

LeanOsWindow::~LeanOsWindow() {
  VLOG(1) << "leanos: window " << widget_ << " destroyed";
  event_watcher_.reset();
  if (buffer_) {
    buffer_->Retire();
    manager_->SetBuffer(widget_, nullptr);
  }
  if (connection_) {
    window_manager_send_action(connection_->window_id, WINDOW_MANAGER_ACTION_CLOSE);
  }
  manager_->RemoveWindow(widget_, this);
}

void LeanOsWindow::Show(bool inactive) {
  VLOG(1) << "leanos: window " << widget_ << " shown"
          << (inactive ? " inactive" : "");
  visible_ = true;
  if (!connection_)
    return;
  // The first action sent from this process opens the action pipe, and the
  // client library keeps that descriptor in a static. The compositor thread
  // will send PRESENT on it from now on, so the open happens here, on the UI
  // thread, before any other thread can race it - and asking for focus is
  // what showing a window means on this desktop anyway.
  window_manager_send_action(connection_->window_id, WINDOW_MANAGER_ACTION_FOCUS);
  if (!inactive)
    Activate();
}

void LeanOsWindow::Hide() {
  VLOG(1) << "leanos: window " << widget_ << " hidden";
  visible_ = false;
}

void LeanOsWindow::Close() {
  VLOG(1) << "leanos: window " << widget_ << " closed";
  delegate_->OnClosed();
}

bool LeanOsWindow::IsVisible() const {
  return visible_;
}

void LeanOsWindow::PrepareForShutdown() {}

void LeanOsWindow::SetBoundsInPixels(const gfx::Rect& bounds) {
  // The compositor does not reallocate a client's buffer, so the size is the
  // one the window was created with; the origin is (0,0) in the window's own
  // space, which is the only space the compositor reports events in. What
  // changes here is nothing, but the delegate still expects to be told.
  bool origin_changed = bounds_.origin() != bounds.origin();
  if (!connection_ || is_popup_) {
    bounds_ = bounds;
  }
  delegate_->OnBoundsChanged({origin_changed});
}

gfx::Rect LeanOsWindow::GetBoundsInPixels() const {
  return bounds_;
}

void LeanOsWindow::SetBoundsInDIP(const gfx::Rect& bounds) {
  SetBoundsInPixels(delegate_->ConvertRectToPixels(bounds));
}

gfx::Rect LeanOsWindow::GetBoundsInDIP() const {
  return delegate_->ConvertRectToDIP(bounds_);
}

void LeanOsWindow::SetTitle(const std::u16string& title) {}

void LeanOsWindow::SetCapture() {
  if (connection_) {
    window_manager_set_capture(connection_.get(), 1);
    has_capture_ = true;
  }
}

void LeanOsWindow::ReleaseCapture() {
  if (connection_ && has_capture_) {
    window_manager_set_capture(connection_.get(), 0);
    has_capture_ = false;
  }
}

bool LeanOsWindow::HasCapture() const {
  return has_capture_;
}

void LeanOsWindow::SetFullscreen(bool fullscreen, int64_t target_display_id) {
  if (!delegate_->CanFullscreen())
    return;
  auto weak_ptr = weak_ptr_factory_.GetWeakPtr();
  if (fullscreen) {
    if (window_state_ != PlatformWindowState::kFullScreen) {
      restored_bounds_ = bounds_;
      UpdateWindowState(PlatformWindowState::kFullScreen);
    }
  } else if (window_state_ == PlatformWindowState::kFullScreen) {
    UpdateWindowState(PlatformWindowState::kNormal);
  }
  if (!weak_ptr)
    return;
}

void LeanOsWindow::Maximize() {
  if (!delegate_->CanMaximize())
    return;
  if (window_state_ != PlatformWindowState::kMaximized) {
    if (connection_)
      window_manager_send_action(connection_->window_id, WINDOW_MANAGER_ACTION_MAXIMIZE);
    UpdateWindowState(PlatformWindowState::kMaximized);
  }
}

void LeanOsWindow::Minimize() {
  if (window_state_ != PlatformWindowState::kMinimized) {
    if (connection_)
      window_manager_send_action(connection_->window_id,
                                 WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE);
    auto weak_ptr = weak_ptr_factory_.GetWeakPtr();
    UpdateWindowState(PlatformWindowState::kMinimized);
    if (!weak_ptr)
      return;
    Deactivate();
  }
}

void LeanOsWindow::Restore() {
  if (window_state_ != PlatformWindowState::kNormal) {
    if (connection_)
      window_manager_send_action(connection_->window_id, WINDOW_MANAGER_ACTION_RESTORE);
    UpdateWindowState(PlatformWindowState::kNormal);
  }
}

PlatformWindowState LeanOsWindow::GetPlatformWindowState() const {
  return window_state_;
}

void LeanOsWindow::Activate() {
  if (!activated_) {
    activated_ = true;
    delegate_->OnActivationChanged(true);
  }
}

void LeanOsWindow::Deactivate() {
  if (activated_) {
    activated_ = false;
    delegate_->OnActivationChanged(false);
  }
}

void LeanOsWindow::SetUseNativeFrame(bool use_native_frame) {}

bool LeanOsWindow::ShouldUseNativeFrame() const {
  // The compositor draws the title bar, the traffic lights and the shadow,
  // which is what a native frame is. Saying so is what stops views from
  // drawing a second frame of its own inside the window.
  return true;
}

void LeanOsWindow::SetCursor(scoped_refptr<PlatformCursor> cursor) {}

void LeanOsWindow::MoveCursorTo(const gfx::Point& location) {}

void LeanOsWindow::ConfineCursorToBounds(const gfx::Rect& bounds) {}

void LeanOsWindow::SetRestoredBoundsInDIP(const gfx::Rect& bounds) {
  restored_bounds_ = delegate_->ConvertRectToPixels(bounds);
}

gfx::Rect LeanOsWindow::GetRestoredBoundsInDIP() const {
  return delegate_->ConvertRectToDIP(restored_bounds_.value_or(bounds_));
}

void LeanOsWindow::SetWindowIcons(const gfx::ImageSkia& window_icon,
                                  const gfx::ImageSkia& app_icon) {}

void LeanOsWindow::SizeConstraintsChanged() {}

void LeanOsWindow::UpdateWindowState(PlatformWindowState new_window_state) {
  if (window_state_ == new_window_state)
    return;
  auto old_window_state = window_state_;
  window_state_ = new_window_state;
  delegate_->OnWindowStateChanged(old_window_state, new_window_state);
}

void LeanOsWindow::OnEventPipeReadable() {
  if (!connection_)
    return;
  auto weak_ptr = weak_ptr_factory_.GetWeakPtr();
  window_manager_event_t event;
  // Drain what is there. poll_event answers 0 for anything short of a whole
  // record, and the watcher fires again when the rest arrives.
  while (weak_ptr && window_manager_poll_event(connection_.get(), &event) == 1) {
    HandleEvent(event);
  }
}

void LeanOsWindow::HandleEvent(const window_manager_event_t& event) {
  switch (event.type) {
    case WINDOW_MANAGER_EVENT_KEY:
      DispatchKey(event.ch, event.mods);
      break;
    case WINDOW_MANAGER_EVENT_MOUSE_MOVE:
    case WINDOW_MANAGER_EVENT_MOUSE_BUTTON:
      DispatchMouse(event.x, event.y, event.buttons, event.mods);
      break;
    case WINDOW_MANAGER_EVENT_MOUSE_WHEEL:
      DispatchWheel(event.x, event.y, event.wheel, event.mods);
      break;
    case WINDOW_MANAGER_EVENT_FOCUS:
      Activate();
      break;
    case WINDOW_MANAGER_EVENT_UNFOCUS:
      Deactivate();
      break;
    case WINDOW_MANAGER_EVENT_CLOSE_REQUEST:
      delegate_->OnCloseRequest();
      break;
    case WINDOW_MANAGER_EVENT_EXPOSE:
    case WINDOW_MANAGER_EVENT_DISPLAY_CHANGED:
      delegate_->OnDamageRect(gfx::Rect(bounds_.size()));
      break;
    default:
      break;
  }
}

void LeanOsWindow::DispatchKey(char ch, uint8_t mods) {
  TranslatedKey key;
  if (!KeyFromCharacter(ch, mods, &key))
    return;
  auto weak_ptr = weak_ptr_factory_.GetWeakPtr();
  base::TimeTicks now = EventTimeForNow();
  KeyEvent pressed(EventType::kKeyPressed, key.key_code, key.dom_code, key.flags,
                   key.dom_key, now);
  delegate_->DispatchEvent(&pressed);
  if (!weak_ptr)
    return;
  KeyEvent released(EventType::kKeyReleased, key.key_code, key.dom_code,
                    key.flags, key.dom_key, now);
  delegate_->DispatchEvent(&released);
}

void LeanOsWindow::DispatchMouse(int32_t x, int32_t y, uint8_t buttons,
                                 uint8_t mods) {
  auto weak_ptr = weak_ptr_factory_.GetWeakPtr();
  gfx::PointF location(x, y);
  last_mouse_ = gfx::Point(x, y);
  manager_->SetCursorScreenPoint(last_mouse_ + bounds_.OffsetFromOrigin());
  base::TimeTicks now = EventTimeForNow();
  const int modifiers = ModifierFlags(mods);
  const uint8_t changed = static_cast<uint8_t>(buttons_ ^ buttons);
  buttons_ = buttons;

  if (!changed) {
    EventType type = buttons ? EventType::kMouseDragged : EventType::kMouseMoved;
    MouseEvent moved(type, location, location, now,
                     ButtonFlags(buttons) | modifiers, 0);
    delegate_->DispatchEvent(&moved);
    return;
  }

  // One record can, in principle, carry two buttons changing at once; each
  // becomes its own press or release. The flags on each are what is held
  // after it PLUS the button that changed - so a release still names the
  // button being let go, which is Chromium's convention (Wayland's event
  // source says so in as many words) and what views::Button asks: it fires
  // on release only when event.flags() includes a button it answers to.
  // M171 left the released button out, and every button that acts on
  // release - New Tab, back, reload, a tab's close box - ignored every
  // click, while everything that acts on press, and every page, worked.
  uint8_t applied = static_cast<uint8_t>(buttons_ ^ changed);
  for (int bit = 0; bit < 3 && weak_ptr; bit++) {
    uint8_t mask = static_cast<uint8_t>(1u << bit);
    if (!(changed & mask))
      continue;
    applied = static_cast<uint8_t>((applied & ~mask) | (buttons & mask));
    bool pressed = (buttons & mask) != 0;
    MouseEvent event(pressed ? EventType::kMousePressed : EventType::kMouseReleased,
                     location, location, now,
                     ButtonFlags(applied | mask) | modifiers, ButtonFlags(mask));
    if (pressed && bit == 0)
      event.SetClickCount(1);
    delegate_->DispatchEvent(&event);
  }
}

void LeanOsWindow::DispatchWheel(int32_t x, int32_t y, int32_t wheel,
                                 uint8_t mods) {
  if (wheel == 0)
    return;
  gfx::PointF location(x, y);
  // The compositor counts a wheel notch towards the user as positive; a
  // ui::MouseWheelEvent counts scrolling UP - the wheel moving away - as a
  // positive y offset, at kWheelDelta pixels a notch. nsfb_leanos.c drew the
  // same line for NetSurf: wheel < 0 is its MOUSE_4, which is scroll up.
  gfx::Vector2d offset(0, -wheel * MouseWheelEvent::kWheelDelta);
  MouseWheelEvent event(offset, location, location, EventTimeForNow(),
                        ButtonFlags(buttons_) | ModifierFlags(mods), 0);
  delegate_->DispatchEvent(&event);
}

}  // namespace ui
