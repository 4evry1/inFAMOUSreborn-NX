#define main spurs_fixture_main_for_tls_snapshot
#include "test_startup_spurs_initialize.cpp"
#undef main
#include "tls_initialize.h"

namespace {
constexpr uint32_t TEMPLATE_SOURCE = 0x007CF100;
constexpr uint32_t IMAGE_BLOCK = 0x007D3000;
constexpr uint64_t IMAGE_EXTENT = 0x9000;
constexpr uint32_t TLS_IMPORT = 0x744680A2;
constexpr uint64_t SOURCE_ARGUMENT = UINT64_C(0xABCD0000007CF100);

struct SnapshotProvider {
    ps3_guest_memory memory{};
    Ps3TlsArena arena{};
    unsigned char scratch[64]{};
    StartupControl *control = nullptr;
    unsigned calls = 0;
    bool cancel_after_initialization = false;
    bool allocated = false;

    static Ps3TlsReport initialize(void *context, const Ps3TlsTemplate *input, Ps3TlsImage *output) {
        auto &provider = *static_cast<SnapshotProvider *>(context);
        ++provider.calls;
        const auto report = ps3_tls_initialize(&provider.memory, &provider.arena, input,
            provider.scratch, sizeof provider.scratch, output);
        provider.allocated = report.status == PS3_TLS_OK;
        if (provider.allocated && provider.cancel_after_initialization)
            provider.control->cancelled.store(true, std::memory_order_release);
        return report;
    }
};

struct SnapshotCase {
    Runtime runtime{1, false, false};
    SnapshotProvider provider;

    explicit SnapshotCase(bool enough_space = true, bool cancel_after = false) {
        auto &r = runtime;
        // Change the declarations before admission; the provider commits the new TLS pages itself.
        r.ranges[0].length = uint64_t(IMAGE_BLOCK) + IMAGE_EXTENT - r.ranges[0].address;
        provider.memory = r.mem();
        provider.arena = {IMAGE_BLOCK, IMAGE_BLOCK + (enough_space ? IMAGE_EXTENT : 0x8000)};
        provider.control = r.control.get();
        provider.cancel_after_initialization = cancel_after;
        const unsigned char bytes[4]{0x18, 0x42, 0xA6, 0x7C};
        r.put(TEMPLATE_SOURCE, bytes, sizeof bytes);
        r.services.context = &provider;
        r.services.initialize_tls = SnapshotProvider::initialize;
        CHECK(native_startup_admit_budgeted(r.execution, *r.binding, r.services, r.recorder, r.budget));
    }

    ~SnapshotCase() {
        if (provider.allocated)
            CHECK(provider.memory.decommit(provider.memory.context, IMAGE_BLOCK, IMAGE_EXTENT) == PS3_GUEST_MEMORY_OK);
    }

    void call(uint64_t source = SOURCE_ARGUMENT, uint64_t file_size = 4, uint64_t memory_size = 0x200) {
        auto &cpu = runtime.execution.ppu.cpu;
        cpu.gpr[4] = source;
        cpu.gpr[5] = file_size;
        cpu.gpr[6] = memory_size;
        runtime.guarded([](Runtime &r) { ps3_hle_call(TLS_IMPORT, &r.execution.ppu.cpu); }, runtime);
    }
};

void check_unpublished(const StartupControl &control) {
    CHECK(!control.initialized_tls_template_valid && control.tls_returns == 0);
    CHECK(control.initialized_tls_template.source == 0);
    CHECK(control.initialized_tls_template.file_size == 0);
    CHECK(control.initialized_tls_template.memory_size == 0);
}

void successful_snapshot_is_stable() {
    SnapshotCase test;
    check_unpublished(*test.runtime.control);
    test.call();
    const auto &control = *test.runtime.control;
    CHECK(control.stop == StartupStop::None && control.tls_returns == 1);
    CHECK(control.initialized_tls_template_valid);
    CHECK(control.initialized_tls_template.source == TEMPLATE_SOURCE);
    CHECK(control.initialized_tls_template.file_size == 4);
    CHECK(control.initialized_tls_template.memory_size == 0x200);
    CHECK(test.provider.calls == 1 && test.provider.allocated);
    CHECK(test.runtime.execution.ppu.cpu.gpr[13] == IMAGE_BLOCK + 0x7000);
    CHECK(test.runtime.get32(IMAGE_BLOCK) == 0x1842A67C);
    CHECK(test.runtime.get8(IMAGE_BLOCK + 4) == 0 && test.runtime.get8(IMAGE_BLOCK + 0x1FF) == 0);

    test.call(TEMPLATE_SOURCE + 8, 7, 0x100);
    CHECK(control.stop == StartupStop::MissingServices && test.provider.calls == 1);
    CHECK(control.initialized_tls_template_valid && control.tls_returns == 1);
    CHECK(control.initialized_tls_template.source == TEMPLATE_SOURCE);
    CHECK(control.initialized_tls_template.file_size == 4 && control.initialized_tls_template.memory_size == 0x200);
}

void failed_provider_does_not_publish() {
    SnapshotCase test(false);
    test.call();
    CHECK(test.runtime.control->stop == StartupStop::ProviderFailure && test.provider.calls == 1);
    CHECK(!test.provider.allocated && test.provider.arena.next == IMAGE_BLOCK);
    check_unpublished(*test.runtime.control);
}

void cancellation_after_provider_does_not_publish() {
    SnapshotCase test(true, true);
    test.call();
    CHECK(test.runtime.control->stop == StartupStop::Cancelled && test.provider.calls == 1);
    CHECK(test.provider.allocated);
    CHECK(!test.runtime.recorder.report().batch_active);
    check_unpublished(*test.runtime.control);
}
}

int main() {
    successful_snapshot_is_stable();
    failed_provider_does_not_publish();
    cancellation_after_provider_does_not_publish();
    std::printf("TLS_SNAPSHOT_HOST failures=%d native_runtime=false game_boot=false\n", failures);
    return failures ? 1 : 0;
}
