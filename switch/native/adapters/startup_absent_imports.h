#ifndef NATIVE_STARTUP_ABSENT_IMPORTS_H
#define NATIVE_STARTUP_ABSENT_IMPORTS_H
#include <cstddef>
#include <cstdint>

/* The game's imports whose answer on this port is fixed because the device or service is absent:
 * USB keyboard, USB mouse, network control, the sys_net network, NP and the sysutil callback poll.
 *
 * The port is offline and has no USB keyboard or mouse. Error values and struct sizes are the
 * coordinator's recollection of the public SDK headers / RPCS3, cross-checked against
 * source/ps3recomp/libs where present; not extracted from firmware. The two netctl values were
 * corrected from source/ps3recomp/libs/network/cellNetCtl.h.
 *
 * One table drives every answer. A row either returns a fixed value or zero-fills one caller-owned
 * record; nothing here is stateful, so every NID costs one lookup and one guest write at most. */

struct StartupAbsentImportMemory {
    void *context;
    // Writes `size` zero bytes at guest address `address`; false when the range is not writable guest memory.
    bool (*zero)(void *context, uint32_t address, uint32_t size);
    // Writes one big-endian u32; false when not writable.
    bool (*store_u32)(void *context, uint32_t address, uint32_t value);
};

enum class StartupAbsentImportStatus { NotHandled, Handled, BadPointer };

struct StartupAbsentImportResult {
    StartupAbsentImportStatus status;
    uint32_t value;   // 32-bit return code as the guest observes it; only meaningful when Handled
};

enum class StartupAbsentImportKind : uint8_t {
    ConstantReturn,   // return `value`; no guest memory is touched
    ZeroOutArgument,  // zero-fill `out_size` bytes at arg0 and return `value`
    StoreU32Argument  // store `value` big-endian at arg0 and return 0
};

constexpr uint32_t startup_absent_error_invalid_parameter_kb = 0x80121002u;
constexpr uint32_t startup_absent_error_invalid_parameter_mouse = 0x80121202u;
constexpr uint32_t startup_absent_error_invalid_parameter_netctl = 0x80130107u;
constexpr uint32_t startup_absent_error_no_device_kb = 0x80121007u;
constexpr uint32_t startup_absent_error_no_device_mouse = 0x80121207u;
constexpr uint32_t startup_absent_error_not_connected_netctl = 0x80130108u;

/* CellKbInfo: max_connect, now_connect, info, status[127], one pad byte = 140 bytes.
 * CellMouseInfo: max_connect, now_connect, info, vendor_id[127], product_id[127], status[127]
 * over four-byte fields, one pad byte = 648 bytes. */
constexpr uint32_t startup_absent_cell_kb_info_bytes = 140u;
constexpr uint32_t startup_absent_cell_mouse_info_bytes = 648u;
constexpr uint32_t startup_absent_net_ctl_state_disconnected = 0u;

struct StartupAbsentImportRow {
    uint32_t nid;
    const char *name;
    StartupAbsentImportKind kind;
    uint32_t value;
    uint32_t invalid_parameter_value;
    uint32_t out_size;
};

// 0x189a74da cellSysutilCheckCallback invokes no callback: no system event is ever pending on this port yet.
constexpr StartupAbsentImportRow startup_absent_import_rows[] = {
    {0x433f6ec0u, "cellKbInit", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0xbfce3285u, "cellKbEnd", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0x2073b7f6u, "cellKbClearBuf", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_kb,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0xa5f85e4du, "cellKbSetCodeType", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_kb,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0xdeefdfa7u, "cellKbSetReadMode", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_kb,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0x3f72c56eu, "cellKbSetLEDStatus", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_kb,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0xff0a21b7u, "cellKbRead", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_kb,
        startup_absent_error_invalid_parameter_kb, 0u},
    {0x2f1774d5u, "cellKbGetInfo", StartupAbsentImportKind::ZeroOutArgument, 0u,
        startup_absent_error_invalid_parameter_kb, startup_absent_cell_kb_info_bytes},
    {0xc9030138u, "cellMouseInit", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_mouse, 0u},
    {0xe10183ceu, "cellMouseEnd", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_mouse, 0u},
    {0x3ef66b95u, "cellMouseClearBuf", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_mouse,
        startup_absent_error_invalid_parameter_mouse, 0u},
    {0x6bd131f0u, "cellMouseGetDataList", StartupAbsentImportKind::ConstantReturn, startup_absent_error_no_device_mouse,
        startup_absent_error_invalid_parameter_mouse, 0u},
    {0x5baf30fbu, "cellMouseGetInfo", StartupAbsentImportKind::ZeroOutArgument, 0u,
        startup_absent_error_invalid_parameter_mouse, startup_absent_cell_mouse_info_bytes},
    {0xbd5a59fcu, "cellNetCtlInit", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0x105ee2cbu, "cellNetCtlTerm", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0x8b3eba69u, "cellNetCtlGetState", StartupAbsentImportKind::StoreU32Argument,
        startup_absent_net_ctl_state_disconnected, startup_absent_error_invalid_parameter_netctl, 4u},
    {0x1e585b5du, "cellNetCtlGetInfo", StartupAbsentImportKind::ConstantReturn,
        startup_absent_error_not_connected_netctl, startup_absent_error_invalid_parameter_netctl, 0u},
    {0x139a9e9bu, "sys_net_initialize_network_ex", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0xb68d5625u, "sys_net_finalize_network", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0xb48636c4u, "sys_net_show_ifconfig", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0xbd28fdbfu, "sceNpInit", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0x4885aa18u, "sceNpTerm", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
    {0x189a74dau, "cellSysutilCheckCallback", StartupAbsentImportKind::ConstantReturn, 0u,
        startup_absent_error_invalid_parameter_netctl, 0u},
};

constexpr size_t startup_absent_import_row_count =
    sizeof(startup_absent_import_rows) / sizeof(startup_absent_import_rows[0]);

inline const StartupAbsentImportRow *startup_absent_import_find(uint32_t nid)
{
    for (size_t index = 0; index < startup_absent_import_row_count; ++index)
        if (startup_absent_import_rows[index].nid == nid) return &startup_absent_import_rows[index];
    return nullptr;
}

inline const char *startup_absent_import_name(uint32_t nid)
{
    const StartupAbsentImportRow *row = startup_absent_import_find(nid);
    return row ? row->name : nullptr;
}

inline StartupAbsentImportResult startup_absent_import(uint32_t nid, uint32_t arg0, uint32_t arg1,
    const StartupAbsentImportMemory &memory)
{
    (void)arg1;   // every absent-device answer depends on its first argument only
    const StartupAbsentImportRow *row = startup_absent_import_find(nid);
    if (!row) return {StartupAbsentImportStatus::NotHandled, 0u};
    if (row->kind == StartupAbsentImportKind::ConstantReturn)
        return {StartupAbsentImportStatus::Handled, row->value};
    if (!arg0)
        return {StartupAbsentImportStatus::Handled, row->invalid_parameter_value};
    if (row->kind == StartupAbsentImportKind::ZeroOutArgument) {
        if (!memory.zero || !memory.zero(memory.context, arg0, row->out_size))
            return {StartupAbsentImportStatus::BadPointer, 0u};
        return {StartupAbsentImportStatus::Handled, row->value};
    }
    if (!memory.store_u32 || !memory.store_u32(memory.context, arg0, row->value))
        return {StartupAbsentImportStatus::BadPointer, 0u};
    return {StartupAbsentImportStatus::Handled, 0u};
}

#endif
