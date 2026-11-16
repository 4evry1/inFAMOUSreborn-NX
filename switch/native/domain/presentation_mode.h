#ifndef NATIVE_PRESENTATION_MODE_H
#define NATIVE_PRESENTATION_MODE_H
#include <cstdint>

// Explicit native logical render-target policy. This is NOT evidence of an
// attached monitor, an initialized GPU, or a presented game frame, and it has
// no PS3 NIDs, guest addresses, traces or platform dependencies.
enum class NativePresentationMode { Unconfigured, Fixed720p };
enum class NativePresentationModeStatus { Configured, Unconfigured, InvalidMode };
struct NativePresentationModeOutcome {
    NativePresentationModeStatus status = NativePresentationModeStatus::Unconfigured;
    uint32_t width = 0, height = 0;
    uint32_t aspect_numerator = 0, aspect_denominator = 0;
};
constexpr NativePresentationModeOutcome presentation_mode_outcome(NativePresentationMode mode) {
    switch (mode) {
    case NativePresentationMode::Unconfigured:
        return {NativePresentationModeStatus::Unconfigured, 0, 0, 0, 0};
    case NativePresentationMode::Fixed720p:
        return {NativePresentationModeStatus::Configured, 1280, 720, 16, 9};
    }
    return {NativePresentationModeStatus::InvalidMode, 0, 0, 0, 0};
}
#endif
