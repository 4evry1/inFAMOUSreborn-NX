#pragma once
#include "../domain/boot_content.h"

// Offline-admitted source disc package; reserved payload zeros are native policy.
inline BootContentMetadata infamous_boot_content_metadata()
{
    return BootContentMetadata{BootContentOrigin::DiscPackage, "BCUS98119", 5u};
}
