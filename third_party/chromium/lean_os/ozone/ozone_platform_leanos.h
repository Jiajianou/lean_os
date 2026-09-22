#ifndef LEAN_OS_OZONE_OZONE_PLATFORM_LEANOS_H_
#define LEAN_OS_OZONE_OZONE_PLATFORM_LEANOS_H_

namespace ui {

class OzonePlatform;

// The name ui/ozone's generated constructor list calls: "leanos" with its
// first letter capitalised, which is generate_constructor_list.py's rule.
OzonePlatform* CreateOzonePlatformLeanos();

}  // namespace ui

#endif  // LEAN_OS_OZONE_OZONE_PLATFORM_LEANOS_H_
