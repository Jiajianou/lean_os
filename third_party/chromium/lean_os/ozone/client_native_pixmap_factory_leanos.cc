#include "lean_os/ozone/client_native_pixmap_factory_leanos.h"

#include "ui/ozone/common/stub_client_native_pixmap_factory.h"

namespace ui {

// A native pixmap is a dma-buf, which is Linux's kernel (patch 0037). The
// stub is the answer every platform without one gives.
gfx::ClientNativePixmapFactory* CreateClientNativePixmapFactoryLeanos() {
  return CreateStubClientNativePixmapFactory();
}

}  // namespace ui
