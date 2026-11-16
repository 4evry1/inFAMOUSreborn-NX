#include <stdio.h>
#include "pad_port_horizon.h"

#define PAD_PORT_HORIZON_SERVED_PORT 0u
#define PAD_PORT_HORIZON_COUNT 1u

/* libnx reports both sticks as s16 in [-32768, 32767]; the guest wants 0..255 with 0x80 centred. A resting stick
 * jitters by a few hundred counts, so anything below this dead zone is read as exactly centred. */
#define PAD_PORT_HORIZON_STICK_DEAD_ZONE 2048
#define PAD_PORT_HORIZON_STICK_BIAS 32768
#define PAD_PORT_HORIZON_STICK_SHIFT 8
#define PAD_PORT_HORIZON_STICK_MIDDLE PAD_ANALOG_CENTRE
#define PAD_PORT_HORIZON_STICK_LOW 0u
#define PAD_PORT_HORIZON_STICK_HIGH 255u

/* Face buttons map by POSITION: the Switch's bottom button is the guest's CROSS, its right one CIRCLE. */
static const struct {
    uint64_t horizon_mask;
    uint16_t digital2_bit;
} PAD_PORT_HORIZON_FACE_BUTTONS[] = {
    {HidNpadButton_B, PAD_CTRL_CROSS},
    {HidNpadButton_A, PAD_CTRL_CIRCLE},
    {HidNpadButton_Y, PAD_CTRL_SQUARE},
    {HidNpadButton_X, PAD_CTRL_TRIANGLE},
    {HidNpadButton_L, PAD_CTRL_L1},
    {HidNpadButton_R, PAD_CTRL_R1},
    {HidNpadButton_ZL, PAD_CTRL_L2},
    {HidNpadButton_ZR, PAD_CTRL_R2},
};

/* The d-pad's own buttons only: libnx's DpadUp..DpadLeft pseudo buttons are derived from the left stick and must not
 * double-report a direction the guest already reads as an analog position. */
static const struct {
    uint64_t horizon_mask;
    uint16_t digital1_bit;
} PAD_PORT_HORIZON_SYSTEM_BUTTONS[] = {
    {HidNpadButton_Minus, PAD_CTRL_SELECT},
    {HidNpadButton_StickL, PAD_CTRL_L3},
    {HidNpadButton_StickR, PAD_CTRL_R3},
    {HidNpadButton_Plus, PAD_CTRL_START},
    {HidNpadButton_Up, PAD_CTRL_UP},
    {HidNpadButton_Right, PAD_CTRL_RIGHT},
    {HidNpadButton_Down, PAD_CTRL_DOWN},
    {HidNpadButton_Left, PAD_CTRL_LEFT},
};

static uint8_t pad_port_horizon_axis(int32_t value, uint32_t is_inverted)
{
    if (value > -PAD_PORT_HORIZON_STICK_DEAD_ZONE && value < PAD_PORT_HORIZON_STICK_DEAD_ZONE) {
        return (uint8_t)PAD_PORT_HORIZON_STICK_MIDDLE;
    }
    int32_t scaled = (value + PAD_PORT_HORIZON_STICK_BIAS) >> PAD_PORT_HORIZON_STICK_SHIFT;
    if (scaled < (int32_t)PAD_PORT_HORIZON_STICK_LOW) scaled = (int32_t)PAD_PORT_HORIZON_STICK_LOW;
    if (scaled > (int32_t)PAD_PORT_HORIZON_STICK_HIGH) scaled = (int32_t)PAD_PORT_HORIZON_STICK_HIGH;
    /* The guest's Y axis grows downwards; Horizon's grows upwards. */
    return (uint8_t)(is_inverted ? (int32_t)PAD_PORT_HORIZON_STICK_HIGH - scaled : scaled);
}

static void pad_port_horizon_map_buttons(uint64_t buttons, pad_host_state *state)
{
    for (size_t index = 0; index < sizeof PAD_PORT_HORIZON_FACE_BUTTONS / sizeof PAD_PORT_HORIZON_FACE_BUTTONS[0];
         ++index) {
        if (buttons & PAD_PORT_HORIZON_FACE_BUTTONS[index].horizon_mask)
            state->digital2 = (uint16_t)(state->digital2 | PAD_PORT_HORIZON_FACE_BUTTONS[index].digital2_bit);
    }
    for (size_t index = 0; index < sizeof PAD_PORT_HORIZON_SYSTEM_BUTTONS / sizeof PAD_PORT_HORIZON_SYSTEM_BUTTONS[0];
         ++index) {
        if (buttons & PAD_PORT_HORIZON_SYSTEM_BUTTONS[index].horizon_mask)
            state->digital1 = (uint16_t)(state->digital1 | PAD_PORT_HORIZON_SYSTEM_BUTTONS[index].digital1_bit);
    }
}

static pad_port_status pad_port_horizon_count(void *context, uint32_t *out_count)
{
    (void)context;
    if (!out_count) return PAD_PORT_ERROR;
    *out_count = PAD_PORT_HORIZON_COUNT;
    return PAD_PORT_OK;
}

static pad_port_status pad_port_horizon_capability(void *context, uint32_t port_no, pad_capability *out)
{
    (void)context;
    if (!out) return PAD_PORT_ERROR;
    if (port_no != PAD_PORT_HORIZON_SERVED_PORT) {
        *out = (pad_capability){0};
        return PAD_PORT_OK;
    }
    *out = (pad_capability){.exists = 1, .supports_press_mode = 1, .supports_sensor_mode = 1};
    return PAD_PORT_OK;
}

/* Root (2026-11-12): unattended runs (the emulator has nobody at the pad). When the file below exists, START is held
 * for 200 ms every 2 s and CROSS for 200 ms one second later, so skippable movies and "press START" screens pass.
 * The file is written by the emulator harness only (EDEN_AUTOPRESS=1); a console run never has it. */
static uint64_t pad_port_horizon_autopress(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        FILE *file = fopen("sdmc:/switch/infamous-native-spu-stage31/autopress.txt", "rb");
        enabled = file != NULL;
        if (file) fclose(file);
    }
    if (!enabled) return 0;
    const uint64_t milliseconds = armTicksToNs(armGetSystemTick()) / 1000000u;
    const uint64_t phase = milliseconds % 2000u;
    if (phase < 200u) return HidNpadButton_Plus;
    if (phase >= 1000u && phase < 1200u) return HidNpadButton_B;
    return 0;
}

static pad_port_status pad_port_horizon_read_state(void *context, uint32_t port_no, pad_host_state *out)
{
    pad_port_horizon *self = (pad_port_horizon *)context;
    if (!out) return PAD_PORT_ERROR;
    if (port_no != PAD_PORT_HORIZON_SERVED_PORT) {
        *out = (pad_host_state){0};
        return PAD_PORT_OK;
    }
    padUpdate(&self->pad);
    ++self->reads;
    uint64_t buttons = padGetButtons(&self->pad);
    buttons |= pad_port_horizon_autopress();
    const HidAnalogStickState left = padGetStickPos(&self->pad, 0);
    const HidAnalogStickState right = padGetStickPos(&self->pad, 1);
    pad_host_state state = {0};
    /* The default pad IS the handheld panel or the first paired controller, so a state always exists here; an
     * unplugged Joy-Con whose half is alone still reports its own buttons. */
    state.connected = 1;
    pad_port_horizon_map_buttons(buttons, &state);
    state.left_x = pad_port_horizon_axis(left.x, 0);
    state.left_y = pad_port_horizon_axis(left.y, 1);
    state.right_x = pad_port_horizon_axis(right.x, 0);
    state.right_y = pad_port_horizon_axis(right.y, 1);
    /* The service turns a pressed digital trigger into full-scale pressure (PAD_PRESSURE_PRESSED). */
    state.l2_analog = 0;
    state.r2_analog = 0;
    *out = state;
    return PAD_PORT_OK;
}

void pad_port_horizon_init(pad_port_horizon *self)
{
    if (!self || self->configured) return;
    padConfigureInput(PAD_PORT_HORIZON_COUNT, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&self->pad);
    self->configured = 1;
}

pad_port pad_port_horizon_port(pad_port_horizon *self)
{
    return (pad_port){self, pad_port_horizon_count, pad_port_horizon_capability, pad_port_horizon_read_state};
}
