#ifndef NATIVE_SYSTEM_PROFILE_H
#define NATIVE_SYSTEM_PROFILE_H
#include <cstddef>

enum class SystemProfileSource { Unconfigured, NativeApplicationProfile };
// Application display configuration only: neither account nor filesystem identity.
struct SystemProfile {
    SystemProfileSource source=SystemProfileSource::Unconfigured;
    char current_user_display_name[64]{};
    char device_display_label[128]{};
};
enum class SystemProfileField { CurrentUserDisplayName, DeviceDisplayLabel };
enum class SystemProfileStatus { Valid, Unconfigured, InvalidSource, InvalidField, Unterminated, InvalidUtf8 };
struct SystemProfileTextValidation {
    SystemProfileStatus status=SystemProfileStatus::Unconfigured;
    std::size_t length=0; // byte length before first NUL, valid only when status is Valid
};
// Validates only the requested field. Empty UTF-8 is valid; bytes after its first
// NUL are ignored. No normalization, truncation, replacement or input mutation.
SystemProfileTextValidation validate_system_profile_text(const SystemProfile &,SystemProfileField);
#endif
