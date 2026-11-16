#ifndef NATIVE_PRESENTATION_TOPOLOGY_H
#define NATIVE_PRESENTATION_TOPOLOGY_H
#include <cstdint>

// Explicit native logical inventory, independent of physical attachment,
// Horizon queries, renderer initialization and presented frames.
enum class PresentationTopology { Unconfigured, SingleNativeTarget };
enum class PresentationInventoryStatus { Configured, Unconfigured, InvalidTopology };
struct PresentationInventory {
    PresentationInventoryStatus status=PresentationInventoryStatus::Unconfigured;
    int32_t primary_count=0,secondary_count=0;
};
constexpr PresentationInventory presentation_inventory(PresentationTopology topology) {
    if(topology==PresentationTopology::Unconfigured) return {};
    if(topology==PresentationTopology::SingleNativeTarget)
        return {PresentationInventoryStatus::Configured,1,0};
    return {PresentationInventoryStatus::InvalidTopology,0,0};
}
#endif
