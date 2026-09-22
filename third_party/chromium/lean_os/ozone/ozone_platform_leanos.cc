// The platform object: what ozone asks for once, at startup, and what each
// answer is on this machine. Mirrors ui/ozone/platform/headless where the
// question has the same answer here - a stub overlay manager, a bitmap
// cursor factory, the minimal input method, the stub keyboard layout - and
// differs where the machine does: the window, the pixels and the screen.

#include "lean_os/ozone/ozone_platform_leanos.h"

#include <memory>

#include "base/no_destructor.h"
#include "build/build_config.h"
#include "lean_os/ozone/leanos_screen.h"
#include "lean_os/ozone/leanos_surface_factory.h"
#include "lean_os/ozone/leanos_window.h"
#include "lean_os/ozone/leanos_window_manager.h"
#include "ui/base/cursor/cursor_factory.h"
#include "ui/base/ime/input_method_minimal.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/ozone/common/bitmap_cursor_factory.h"
#include "ui/ozone/common/stub_overlay_manager.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/input_controller.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/stub_input_controller.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

namespace {

// Something has to be the PlatformEventSource; the events themselves come in
// through each window's pipe rather than through a source Chromium polls.
class LeanOsPlatformEventSource : public PlatformEventSource {
 public:
  LeanOsPlatformEventSource() = default;
  LeanOsPlatformEventSource(const LeanOsPlatformEventSource&) = delete;
  LeanOsPlatformEventSource& operator=(const LeanOsPlatformEventSource&) = delete;
  ~LeanOsPlatformEventSource() override = default;
};

class OzonePlatformLeanOs : public OzonePlatform {
 public:
  OzonePlatformLeanOs() = default;
  OzonePlatformLeanOs(const OzonePlatformLeanOs&) = delete;
  OzonePlatformLeanOs& operator=(const OzonePlatformLeanOs&) = delete;
  ~OzonePlatformLeanOs() override = default;

  // OzonePlatform:
  ui::SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surface_factory_.get();
  }
  OverlayManagerOzone* GetOverlayManager() override {
    return overlay_manager_.get();
  }
  CursorFactory* GetCursorFactory() override { return cursor_factory_.get(); }
  InputController* GetInputController() override {
    return input_controller_.get();
  }
  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return gpu_platform_support_host_.get();
  }
  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }
  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    return std::make_unique<LeanOsWindow>(delegate, window_manager_.get(),
                                          properties);
  }
  bool IsWindowCompositingSupported() const override { return true; }
  std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate()
      override {
    return nullptr;
  }
  std::unique_ptr<PlatformScreen> CreateScreen() override {
    return std::make_unique<LeanOsScreen>(window_manager_.get());
  }
  void InitScreen(PlatformScreen* screen) override {}
  std::unique_ptr<InputMethod> CreateInputMethod(
      ImeKeyEventDispatcher* ime_key_event_dispatcher,
      gfx::AcceleratedWidget widget) override {
    return std::make_unique<InputMethodMinimal>(ime_key_event_dispatcher);
  }

  const PlatformProperties& GetPlatformProperties() override {
    static base::NoDestructor<OzonePlatform::PlatformProperties> properties;
    return *properties;
  }

  bool InitializeUI(const InitParams& params) override {
    window_manager_ = std::make_unique<LeanOsWindowManager>();
    surface_factory_ =
        std::make_unique<LeanOsSurfaceFactory>(window_manager_.get());
    if (!PlatformEventSource::GetInstance())
      platform_event_source_ = std::make_unique<LeanOsPlatformEventSource>();
    keyboard_layout_engine_ = std::make_unique<StubKeyboardLayoutEngine>();
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(
        keyboard_layout_engine_.get());
    overlay_manager_ = std::make_unique<StubOverlayManager>();
    input_controller_ = std::make_unique<StubInputController>();
    cursor_factory_ = std::make_unique<BitmapCursorFactory>();
    gpu_platform_support_host_.reset(CreateStubGpuPlatformSupportHost());
    return true;
  }

  void InitializeGPU(const InitParams& params) override {
    // In one process the UI half has already made the factory, and the
    // window manager the canvases look their buffers up in. In a GPU process
    // of its own there would be no windows on this side to draw into - a
    // frame would have to cross a process boundary, which is the rung M169
    // named and this platform does not claim.
    if (!surface_factory_) {
      if (!window_manager_)
        window_manager_ = std::make_unique<LeanOsWindowManager>();
      surface_factory_ =
          std::make_unique<LeanOsSurfaceFactory>(window_manager_.get());
    }
  }

 private:
  std::unique_ptr<KeyboardLayoutEngine> keyboard_layout_engine_;
  std::unique_ptr<LeanOsWindowManager> window_manager_;
  std::unique_ptr<LeanOsSurfaceFactory> surface_factory_;
  std::unique_ptr<PlatformEventSource> platform_event_source_;
  std::unique_ptr<CursorFactory> cursor_factory_;
  std::unique_ptr<InputController> input_controller_;
  std::unique_ptr<GpuPlatformSupportHost> gpu_platform_support_host_;
  std::unique_ptr<OverlayManagerOzone> overlay_manager_;
};

}  // namespace

OzonePlatform* CreateOzonePlatformLeanos() {
  return new OzonePlatformLeanOs();
}

}  // namespace ui
