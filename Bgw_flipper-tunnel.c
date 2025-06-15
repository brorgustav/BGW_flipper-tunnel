#include "furi.h"
#include "gui/gui.h"
#include "furi_hal.h"
#include "ir.h"

#define SLAVE_ADDR 0x50
#define CMD_LEN    2
static const uint8_t i2c_cmd[CMD_LEN] = {0x10, 0xFF};

typedef struct {
    bool i2c_enabled;
} TunnelAppState;

// Menu button callback: toggles I²C on/off
static void tunnel_menu_callback(View* view, uint32_t context) {
    TunnelAppState* state = view_get_context(view);
    bool new_val = !state->i2c_enabled;
    furi_message_queue_put((FuriMessageQueue*)context, &new_val, 0);
}

// Draw callback: shows current status on screen
static void tunnel_draw_callback(Canvas* canvas, void* ctx) {
    TunnelAppState* state = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "bgw tunnel");
    canvas_draw_str(canvas, 0, 30, state->i2c_enabled ? "I2C: ON" : "I2C: OFF");
    canvas_draw_str(canvas, 0, 50, "Press OK to toggle");
}

int32_t bgw_flipper_tunnel_app(void* p) {
    UNUSED(p);
    TunnelAppState state = { .i2c_enabled = false };

    // Initialize hardware UART
    FuriHalUartSettings uart_settings = {
        .baud_rate = 115200,
        .data_bits = FuriHalUartDataBits8,
        .stop_bits = FuriHalUartStopBits1,
        .parity = FuriHalUartParityNone
    };
    FuriHalUart* uart = furi_hal_uart_init(FuriHalUartIdMain, &uart_settings);

    // Set up menu view
    FuriMessageQueue* menu_queue = furi_message_queue_alloc(1, sizeof(bool));
    View* view = view_alloc();
    view_set_context(view, &state);
    view_set_menu_callback(view, tunnel_menu_callback, (uint32_t)menu_queue);
    view_set_draw_callback(view, tunnel_draw_callback, &state);
    view_open(view);

    // Initialize I²C master
    furi_hal_i2c_init();
    furi_hal_io_init(); // ensure I2C pins are configured
    furi_hal_i2c_acquire(&FuriHalI2cHandleI2c);

    // Main loop: check menu, optionally send I²C, always read IR
    while(1) {
        bool new_val;
        if(furi_message_queue_get(menu_queue, &new_val, 0) == FuriStatusOk) {
            state.i2c_enabled = new_val;
            char status_msg[32];
            snprintf(status_msg, sizeof(status_msg), "I2C %s\r\n",
                     state.i2c_enabled ? "Enabled" : "Disabled");
            furi_hal_uart_transmit(uart, (uint8_t*)status_msg, strlen(status_msg), FuriHalWaitForever);
        }

        if(state.i2c_enabled) {
            bool ok = furi_hal_i2c_tx(&FuriHalI2cHandleI2c, SLAVE_ADDR, (uint8_t*)i2c_cmd, CMD_LEN, 1000);
            char tx_msg[32];
            snprintf(tx_msg, sizeof(tx_msg), "I2C send %s\r\n", ok ? "OK" : "FAIL");
            furi_hal_uart_transmit(uart, (uint8_t*)tx_msg, strlen(tx_msg), FuriHalWaitForever);
        }

        uint16_t raw[64];
        size_t count = 0;
        ir_read_raw(raw, &count, 64, 1000);
        for(size_t i = 0; i < count; i++) {
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "%u%s", raw[i], (i+1<count)?",":"\r\n");
            furi_hal_uart_transmit(uart, (uint8_t*)buf, len, FuriHalWaitForever);
        }

        furi_delay_ms(500);
    }

    // Clean up (unreachable in current infinite loop)
    furi_hal_uart_deinit(uart);
    furi_hal_i2c_release(&FuriHalI2cHandleI2c);
    furi_hal_i2c_deinit_early();
    view_free(view);
    furi_message_queue_free(menu_queue);
    return 0;
}
