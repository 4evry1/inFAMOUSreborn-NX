/* Decision 34: the imports whose answer on this port is fixed because the device or service is
 * absent (USB keyboard, USB mouse, network control, sys_net, NP, the sysutil callback poll).
 *
 * Host test only: a fake 64 KiB guest memory behind the two callbacks of startup_absent_imports.h.
 * No Switch header, no game, no network, no real guest memory. */
#include "../adapters/startup_absent_imports.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kGuestBytes = 64u * 1024u;
constexpr uint32_t kRecordBase = 0x100u;
constexpr unsigned char kFill = 0xA5;
constexpr uint32_t kUnknownNid = 0x12345678u;
constexpr uint32_t kCellPadGetDataNid = 0x8b72cda1u;

struct FakeGuest {
    unsigned char bytes[kGuestBytes];
    uint32_t zero_calls = 0;
    uint32_t store_calls = 0;
};

bool zero_guest(void *context, uint32_t address, uint32_t size)
{
    FakeGuest &guest = *static_cast<FakeGuest *>(context);
    if (address > kGuestBytes || size > kGuestBytes - address) return false;
    ++guest.zero_calls;
    std::memset(guest.bytes + address, 0, size);
    return true;
}

bool store_guest_u32(void *context, uint32_t address, uint32_t value)
{
    FakeGuest &guest = *static_cast<FakeGuest *>(context);
    if (address > kGuestBytes || 4u > kGuestBytes - address) return false;
    ++guest.store_calls;
    guest.bytes[address + 0] = static_cast<unsigned char>(value >> 24);
    guest.bytes[address + 1] = static_cast<unsigned char>(value >> 16);
    guest.bytes[address + 2] = static_cast<unsigned char>(value >> 8);
    guest.bytes[address + 3] = static_cast<unsigned char>(value);
    return true;
}

StartupAbsentImportMemory memory_port(FakeGuest &guest)
{
    return StartupAbsentImportMemory{&guest, zero_guest, store_guest_u32};
}

int failures = 0;

void check(bool condition, const char *what, int line)
{
    if (condition) return;
    std::printf("STARTUP_ABSENT_IMPORTS_FAIL line=%d what=%s\n", line, what);
    ++failures;
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void fill_guest(FakeGuest &guest)
{
    std::memset(guest.bytes, kFill, sizeof(guest.bytes));
}

bool guest_is_fill(const FakeGuest &guest, uint32_t address, uint32_t size)
{
    for (uint32_t index = 0; index < size; ++index)
        if (guest.bytes[address + index] != kFill) return false;
    return true;
}

bool guest_is_zero(const FakeGuest &guest, uint32_t address, uint32_t size)
{
    for (uint32_t index = 0; index < size; ++index)
        if (guest.bytes[address + index] != 0) return false;
    return true;
}

void expect_constant_row(uint32_t nid, uint32_t expected, FakeGuest &guest)
{
    fill_guest(guest);
    guest.zero_calls = 0;
    guest.store_calls = 0;
    const StartupAbsentImportResult result =
        startup_absent_import(nid, kRecordBase, 0u, memory_port(guest));
    CHECK(result.status == StartupAbsentImportStatus::Handled);
    CHECK(result.value == expected);
    CHECK(guest.zero_calls == 0);
    CHECK(guest.store_calls == 0);
    CHECK(guest_is_fill(guest, 0, kGuestBytes));
}

void expect_zero_out_row(uint32_t nid, uint32_t size, FakeGuest &guest)
{
    fill_guest(guest);
    const StartupAbsentImportResult result =
        startup_absent_import(nid, kRecordBase, 0u, memory_port(guest));
    CHECK(result.status == StartupAbsentImportStatus::Handled);
    CHECK(result.value == 0u);
    CHECK(guest_is_zero(guest, kRecordBase, size));
    CHECK(guest.bytes[kRecordBase + size] == kFill);
    CHECK(guest.bytes[kRecordBase - 1] == kFill);
}

void expect_not_handled(uint32_t nid, FakeGuest &guest)
{
    fill_guest(guest);
    const StartupAbsentImportResult result =
        startup_absent_import(nid, kRecordBase, 0u, memory_port(guest));
    CHECK(result.status == StartupAbsentImportStatus::NotHandled);
    CHECK(guest_is_fill(guest, 0, kGuestBytes));
}

void test_table_rows_are_unique_and_named()
{
    for (size_t index = 0; index < startup_absent_import_row_count; ++index) {
        const StartupAbsentImportRow &row = startup_absent_import_rows[index];
        CHECK(row.name != nullptr);
        CHECK(startup_absent_import_name(row.nid) == row.name);
        for (size_t other = index + 1; other < startup_absent_import_row_count; ++other)
            CHECK(row.nid != startup_absent_import_rows[other].nid);
    }
    CHECK(startup_absent_import_row_count == 23u);
    CHECK(startup_absent_import_name(kUnknownNid) == nullptr);
    CHECK(startup_absent_import_name(kCellPadGetDataNid) == nullptr);
}

void test_constant_rows(FakeGuest &guest)
{
    expect_constant_row(0x433f6ec0u, 0u, guest);                                  // cellKbInit
    expect_constant_row(0xbfce3285u, 0u, guest);                                  // cellKbEnd
    expect_constant_row(0x2073b7f6u, 0x80121007u, guest);                         // cellKbClearBuf
    expect_constant_row(0xa5f85e4du, 0x80121007u, guest);                         // cellKbSetCodeType
    expect_constant_row(0xdeefdfa7u, 0x80121007u, guest);                         // cellKbSetReadMode
    expect_constant_row(0x3f72c56eu, 0x80121007u, guest);                         // cellKbSetLEDStatus
    expect_constant_row(0xff0a21b7u, 0x80121007u, guest);                         // cellKbRead
    expect_constant_row(0xc9030138u, 0u, guest);                                  // cellMouseInit
    expect_constant_row(0xe10183ceu, 0u, guest);                                  // cellMouseEnd
    expect_constant_row(0x3ef66b95u, 0x80121207u, guest);                         // cellMouseClearBuf
    expect_constant_row(0x6bd131f0u, 0x80121207u, guest);                         // cellMouseGetDataList
    expect_constant_row(0xbd5a59fcu, 0u, guest);                                  // cellNetCtlInit
    expect_constant_row(0x105ee2cbu, 0u, guest);                                  // cellNetCtlTerm
    expect_constant_row(0x1e585b5du, 0x80130108u, guest);                         // cellNetCtlGetInfo
    expect_constant_row(0x139a9e9bu, 0u, guest);                                  // sys_net_initialize_network_ex
    expect_constant_row(0xb68d5625u, 0u, guest);                                  // sys_net_finalize_network
    expect_constant_row(0xb48636c4u, 0u, guest);                                  // sys_net_show_ifconfig
    expect_constant_row(0xbd28fdbfu, 0u, guest);                                  // sceNpInit
    expect_constant_row(0x4885aa18u, 0u, guest);                                  // sceNpTerm
    expect_constant_row(0x189a74dau, 0u, guest);                                  // cellSysutilCheckCallback
}

void test_record_rows(FakeGuest &guest)
{
    expect_zero_out_row(0x2f1774d5u, startup_absent_cell_kb_info_bytes, guest);   // cellKbGetInfo
    expect_zero_out_row(0x5baf30fbu, startup_absent_cell_mouse_info_bytes, guest); // cellMouseGetInfo
    CHECK(startup_absent_cell_kb_info_bytes == 140u);
    CHECK(startup_absent_cell_mouse_info_bytes == 648u);

    fill_guest(guest);
    guest.zero_calls = 0;
    guest.store_calls = 0;
    const StartupAbsentImportResult result =
        startup_absent_import(0x8b3eba69u, kRecordBase, 0u, memory_port(guest));  // cellNetCtlGetState
    CHECK(result.status == StartupAbsentImportStatus::Handled);
    CHECK(result.value == 0u);
    CHECK(guest.zero_calls == 0);
    CHECK(guest.store_calls == 1);
    CHECK(guest.bytes[kRecordBase + 0] == 0 && guest.bytes[kRecordBase + 1] == 0);
    CHECK(guest.bytes[kRecordBase + 2] == 0 && guest.bytes[kRecordBase + 3] == 0);
    CHECK(guest.bytes[kRecordBase + 4] == kFill);
    CHECK(guest.bytes[kRecordBase - 1] == kFill);
}

void test_null_out_pointer_is_invalid_parameter(FakeGuest &guest)
{
    struct NullCase { uint32_t nid; uint32_t expected; };
    const NullCase cases[] = {
        {0x2f1774d5u, startup_absent_error_invalid_parameter_kb},     // cellKbGetInfo
        {0x5baf30fbu, startup_absent_error_invalid_parameter_mouse},  // cellMouseGetInfo
        {0x8b3eba69u, startup_absent_error_invalid_parameter_netctl}, // cellNetCtlGetState
    };
    for (const NullCase &item : cases) {
        fill_guest(guest);
        guest.zero_calls = 0;
        guest.store_calls = 0;
        const StartupAbsentImportResult result =
            startup_absent_import(item.nid, 0u, 0u, memory_port(guest));
        CHECK(result.status == StartupAbsentImportStatus::Handled);
        CHECK(result.value == item.expected);
        CHECK(guest.zero_calls == 0);
        CHECK(guest.store_calls == 0);
        CHECK(guest_is_fill(guest, 0, kGuestBytes));
    }
}

void test_refused_pointer_is_bad_pointer(FakeGuest &guest)
{
    struct RefusedCase { uint32_t nid; uint32_t address; };
    const RefusedCase cases[] = {
        {0x2f1774d5u, kGuestBytes - 100u},  // cellKbGetInfo, 140 bytes would run past the end
        {0x5baf30fbu, kGuestBytes - 600u},  // cellMouseGetInfo, 648 bytes would run past the end
        {0x8b3eba69u, kGuestBytes - 2u},    // cellNetCtlGetState, 4 bytes would run past the end
    };
    for (const RefusedCase &item : cases) {
        fill_guest(guest);
        const StartupAbsentImportResult result =
            startup_absent_import(item.nid, item.address, 0u, memory_port(guest));
        CHECK(result.status == StartupAbsentImportStatus::BadPointer);
        CHECK(guest_is_fill(guest, 0, kGuestBytes));
    }
}

void test_absent_callbacks_refuse_rather_than_silently_succeed()
{
    const StartupAbsentImportMemory empty{nullptr, nullptr, nullptr};
    CHECK(startup_absent_import(0x2f1774d5u, kRecordBase, 0u, empty).status
        == StartupAbsentImportStatus::BadPointer);
    CHECK(startup_absent_import(0x8b3eba69u, kRecordBase, 0u, empty).status
        == StartupAbsentImportStatus::BadPointer);
    CHECK(startup_absent_import(0x433f6ec0u, 0u, 0u, empty).status
        == StartupAbsentImportStatus::Handled);
}

}   // namespace

int main()
{
    FakeGuest guest;
    int cases = 0;
    const int before = failures;

    test_table_rows_are_unique_and_named(); ++cases;
    test_constant_rows(guest); ++cases;
    test_record_rows(guest); ++cases;
    test_null_out_pointer_is_invalid_parameter(guest); ++cases;
    test_refused_pointer_is_bad_pointer(guest); ++cases;
    test_absent_callbacks_refuse_rather_than_silently_succeed(); ++cases;
    expect_not_handled(kUnknownNid, guest); ++cases;
    expect_not_handled(kCellPadGetDataNid, guest); ++cases;

    if (failures != before) {
        std::printf("STARTUP_ABSENT_IMPORTS_FAIL cases=%d failures=%d\n", cases, failures);
        return 1;
    }
    std::printf("STARTUP_ABSENT_IMPORTS_PASS cases=%d\n", cases);
    return 0;
}
