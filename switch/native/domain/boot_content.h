#ifndef NATIVE_BOOT_CONTENT_H
#define NATIVE_BOOT_CONTENT_H
#include <cstdint>

enum class BootContentOrigin { Unavailable,DiscPackage,InstalledPackage };
// Semantic metadata of the selected launch package, copied into each execution.
// This is not a guest ABI layout or evidence of drive/VFS/mount availability.
struct BootContentMetadata {
    BootContentOrigin origin=BootContentOrigin::Unavailable;
    char title_id[10]{};
    uint32_t parental_level=0;
};
#endif
