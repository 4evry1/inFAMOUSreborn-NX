/* Focused host test for the native cellVideoOutConfigure consumed view through the
 * REAL ps3_hle_call dispatcher and the owned StartupExecution/Runtime. It tests the
 * bridge policy (the game's traced 720p logical target), not the game, hardware, a
 * monitor or a rendered frame. */
#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t CONFIGURE_NID = 0x0bae8772;
constexpr uint64_t CONFIGURE_LR = 0x003aab44;
constexpr uint32_t CONFIGURE = STACK + 0x8C;
constexpr size_t RECORD_BYTES = 16;
constexpr uint32_t PITCH = 5120;   // 0x1400
constexpr uint64_t SENTINEL = UINT64_C(0xa5a5a5a5a5a5a5a5);

void configured(Runtime &r) {
    r.execution.presentation_topology = PresentationTopology::SingleNativeTarget;
    r.execution.presentation_mode = NativePresentationMode::Fixed720p;
}
void seed_toc(Runtime &r) {
    const unsigned char b[8]{0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
    r.put(TOC_SLOT, b, 8);
}
void make_record(unsigned char out[RECORD_BYTES], unsigned char aspect = 0,
    uint16_t pitch = PITCH) {
    std::memset(out, 0, RECORD_BYTES);
    out[0] = 2;   // resolutionId 720p
    out[1] = 0;   // format X8R8G8B8
    out[2] = aspect;
    out[14] = uint8_t(pitch >> 8);
    out[15] = uint8_t(pitch);
}
void seed_record(Runtime &r, const unsigned char record[RECORD_BYTES]) {
    r.put(CONFIGURE, record, RECORD_BYTES);
}
std::vector<unsigned char> read_record(Runtime &r) {
    std::vector<unsigned char> b(RECORD_BYTES);
    CHECK(r.mem().read(r.mem().context, CONFIGURE, b.data(), b.size()) == PS3_GUEST_MEMORY_OK);
    return b;
}
std::vector<unsigned char> payload_snapshot(Runtime &r) {
    std::vector<unsigned char> out;
    for (const auto &range : r.ranges) {
        std::vector<unsigned char> b(range.length);
        CHECK(r.mem().read(r.mem().context, range.address, b.data(), b.size()) == PS3_GUEST_MEMORY_OK);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}
size_t snapshot_toc_offset(Runtime &r) {
    size_t base = 0;
    for (const auto &range : r.ranges) {
        if (TOC_SLOT >= range.address &&
            uint64_t(TOC_SLOT) - range.address + 8 <= range.length)
            return base + (TOC_SLOT - range.address);
        base += range.length;
    }
    return SIZE_MAX;
}
bool same_except_toc(const std::vector<unsigned char> &a,
    const std::vector<unsigned char> &b, size_t toc) {
    if (a.size() != b.size() || toc == SIZE_MAX || toc + 8 > a.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if ((i < toc || i >= toc + 8) && a[i] != b[i]) return false;
    return true;
}
void call_configure(Runtime &r, uint64_t video_out = 0, uint64_t record = CONFIGURE,
    uint64_t option = 0, uint64_t wait = 0, uint64_t lr = CONFIGURE_LR) {
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[1] = STACK; cpu.gpr[2] = TOC; cpu.gpr[3] = video_out; cpu.gpr[4] = record;
    cpu.gpr[5] = option; cpu.gpr[6] = wait;
    for (unsigned i = 7; i <= 10; ++i) cpu.gpr[i] = UINT64_C(0xffffaaaa00000000) + i;
    cpu.lr = lr; cpu.cr = 0x12345678; r.run(CONFIGURE_NID, false);
}
void configure_success_and_exact_effect() {
    // The traced call for both accepted aspects (AUTO and 16:9).
    for (const unsigned char aspect : {0, 2}) {
        Runtime r; configured(r); seed_toc(r);
        unsigned char record[RECORD_BYTES]; make_record(record, aspect);
        seed_record(r, record);
        const auto before = payload_snapshot(r);
        const size_t toc = snapshot_toc_offset(r);
        const size_t from = r.published();
        call_configure(r);
        const auto &o = r.control->video_configure;
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC);
        CHECK(r.control->video_logical_target_configured);
        CHECK(o.failure == StartupVideoConfigureFailure::None && o.has_result && o.toc_written);
        CHECK(o.lr == CONFIGURE_LR && o.raw_r3 == 0 && o.raw_r4 == CONFIGURE &&
              o.raw_r5 == 0 && o.raw_r6 == 0);
        CHECK(std::memcmp(o.record, record, RECORD_BYTES) == 0);
        CHECK(o.publications == 1);
        CHECK(r.get(TOC_SLOT, 8) == TOC);
        CHECK(same_except_toc(before, payload_snapshot(r), toc));
        CHECK(read_record(r) == std::vector<unsigned char>(record, record + RECORD_BYTES));
        CHECK(r.since(from).size() == 1);
        CHECK(r.control->video_configure_calls == 1 && r.control->video_configure_returns == 1);
        CHECK(r.execution.ppu.cpu.lr == CONFIGURE_LR && r.execution.ppu.cpu.cr == 0x12345678);
        CHECK(r.execution.ppu.cpu.gpr[4] == CONFIGURE);
        for (unsigned i = 7; i <= 10; ++i)
            CHECK(r.execution.ppu.cpu.gpr[i] == UINT64_C(0xffffaaaa00000000) + i);
    }
}
void configure_rejects_unsupported_boundaries() {
    for (unsigned s = 0; s < 14; ++s) {
        Runtime r; configured(r); seed_toc(r);
        unsigned char record[RECORD_BYTES]; make_record(record);
        uint64_t video_out = 0, record_ea = CONFIGURE, option = 0, wait = 0, lr = CONFIGURE_LR;
        StartupVideoConfigureFailure failure = StartupVideoConfigureFailure::None;
        StartupStop stop = StartupStop::VideoConfigureStop;
        switch (s) {
        case 0: lr = 0x003aab40; failure = StartupVideoConfigureFailure::WrongLr; break;
        case 1: video_out = 1; failure = StartupVideoConfigureFailure::WrongVideoOut; break;
        case 2: record_ea = UINT64_C(0x100000000) + CONFIGURE;
            failure = StartupVideoConfigureFailure::HighBits; break;
        case 3: record_ea = UINT32_MAX - 7;
            failure = StartupVideoConfigureFailure::InvalidExtent;
            stop = StartupStop::InvalidMemory; break;
        case 4: record[0] = 1; failure = StartupVideoConfigureFailure::UnsupportedConfiguration; break;
        case 5: record[1] = 1; failure = StartupVideoConfigureFailure::UnsupportedConfiguration; break;
        case 6: record[2] = 1; failure = StartupVideoConfigureFailure::UnsupportedConfiguration; break;
        case 7: record[5] = 1; failure = StartupVideoConfigureFailure::UnsupportedConfiguration; break;
        case 8: make_record(record, 0, 5124); failure = StartupVideoConfigureFailure::UnsupportedConfiguration; break;
        case 9: option = 1; failure = StartupVideoConfigureFailure::UnsupportedOption; break;
        case 10: wait = 1; failure = StartupVideoConfigureFailure::UnsupportedOption; break;
        case 11: r.execution.presentation_topology = PresentationTopology::Unconfigured;
            failure = StartupVideoConfigureFailure::UnsupportedTopology; break;
        case 12: r.execution.presentation_mode = NativePresentationMode::Unconfigured;
            failure = StartupVideoConfigureFailure::UnsupportedMode; break;
        case 13: r.execution.presentation_mode = static_cast<NativePresentationMode>(99);
            failure = StartupVideoConfigureFailure::UnsupportedMode; break;
        }
        seed_record(r, record);
        const auto before = payload_snapshot(r);
        const auto record_before = read_record(r);
        const size_t from = r.published();
        call_configure(r, video_out, record_ea, option, wait, lr);
        const auto &o = r.control->video_configure;
        CHECK(r.control->stop == stop);
        CHECK(o.failure == failure);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(!r.control->video_configure_returns);
        CHECK(!r.control->video_logical_target_configured);
        CHECK(r.get(TOC_SLOT, 8) == SENTINEL);
        CHECK(payload_snapshot(r) == before);
        CHECK(read_record(r) == record_before);
        CHECK(r.since(from).empty());
        CHECK(r.execution.ppu.cpu.gpr[3] == video_out);
        CHECK(r.execution.ppu.cpu.gpr[2] == TOC);
        // Only the cases that reach the record read observe it.
        if (s >= 4 && s <= 10)
            CHECK(std::memcmp(o.record, record, RECORD_BYTES) == 0);
    }
}
void configure_trace_capacity_and_cancellation() {
    {
        Runtime r; configured(r); seed_toc(r);
        unsigned char record[RECORD_BYTES]; make_record(record); seed_record(r, record);
        r.leave_trace_capacity(0);
        call_configure(r);
        const auto &o = r.control->video_configure;
        CHECK(r.control->stop == StartupStop::TraceCapacity);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(!r.control->video_configure_returns && r.get(TOC_SLOT, 8) == SENTINEL);
        CHECK(!r.control->video_logical_target_configured);
    }
    {
        Runtime r; configured(r); seed_toc(r);
        unsigned char record[RECORD_BYTES]; make_record(record); seed_record(r, record);
        r.control->cancelled.store(true);
        call_configure(r);
        const auto &o = r.control->video_configure;
        CHECK(r.control->stop == StartupStop::Cancelled);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(!r.control->video_configure_returns && r.get(TOC_SLOT, 8) == SENTINEL);
        CHECK(!r.control->video_logical_target_configured);
    }
}
}
int main() {
    configure_success_and_exact_effect();
    configure_rejects_unsupported_boundaries();
    configure_trace_capacity_and_cancellation();
    std::printf("VIDEO_CONFIGURE_BRIDGE_FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}
