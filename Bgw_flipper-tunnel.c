#include "furi.h"
#include "gui/gui.h"
#include "furi_hal.h"
#include "ir.h"

#define SLAVE_ADDR 0x50
#define CMD_LEN    2
static const uint8_t i2c_cmd[CMD_LEN] = {0x10, 0xFF};

typedef struct {
    bool i2c_enabled;
    bool use_usb;           // true → USB serial, false → HW UART
} TunnelAppState;

// Toggle USB/HW serial
static void tunnel_switch_serial(View* view, uint32_t context) {
    TunnelAppState* state = view_get_context(view);
    state->use_usb = !state->use_usb;
}

// Toggle I²C transmission
static void tunnel_toggle_i2c(View* view, uint32_t context) {
    TunnelAppState* state = view_get_context(view);
    state->i2c_enabled = !state->i2c_enabled;
}

// Draw the menu
static void tunnel_draw(Canvas* canvas, void* ctx) {
    TunnelAppState* s = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 5, "bgw tunnel");
    canvas_draw_str(canvas, 0, 20, s->i2c_enabled ? "I2C: ON" : "I2C: OFF");
    canvas_draw_str(canvas, 0, 35, s->use_usb ? "Serial: USB" : "Serial: HW");
    canvas_draw_str(canvas, 0, 50, "OK=I2C, MENU=Serial");
}

int32_t bgw_flipper_tunnel_app(void* p) {
    UNUSED(p);
    TunnelAppState state = {0};

    // Init I²C
    furi_hal_i2c_init();
    furi_hal_io_init();
    furi_hal_i2c_acquire(&FuriHalI2cHandleI2c);

    // UART handles
    FuriHalUartSettings uset = {
        .baud_rate = 115200,
        .data_bits = FuriHalUartDataBits8,
        .stop_bits = FuriHalUartStopBits1,
        .parity    = FuriHalUartParityNone
    };
    FuriHalUart* uart_hw = furi_hal_uart_init(FuriHalUartIdMain, &uset);
    FuriHalUart* uart_usb = furi_hal_uart_init(FuriHalUartIdUsb, &uset);

    // Setup UI
    View* view = view_alloc();
    view_set_context(view, &state);
    view_set_draw_callback(view, tunnel_draw, &state);
    view_set_input_callback(view, [](View* v, FuriEvent* e, void* ctx){
        TunnelAppState* s = ctx;
        if(e->type == FuriEventTypePress) {
            if(e->key == FuriKeyOk) tunnel_toggle_i2c(v, 0);
            else if(e->key == FuriKeyBack) {} 
            else if(e->key == FuriKeyOff) {}
            else if(e->key == FuriKeyMenu) tunnel_switch_serial(v, 0);
        }
    }, &state);
    view_open(view);

    while(1) {
        // I²C if enabled
        if(state.i2c_enabled) {
            bool ok = furi_hal_i2c_tx(&FuriHalI2cHandleI2c, SLAVE_ADDR, (uint8_t*)i2c_cmd, CMD_LEN, 1000);
            const char* resp = ok ? "I2C OK\r\n" : "I2C FAIL\r\n";
            furi_hal_uart_transmit(state.use_usb ? uart_usb : uart_hw, (uint8_t*)resp, strlen(resp), FuriHalWaitForever);
        }
        // IR pulses
        uint16_t raw[64]; size_t count=0;
        ir_read_raw(raw, &count, 64, 1000);
        for(size_t i=0;i<count;i++){
            char buf[20];
            int l=snprintf(buf,sizeof(buf),"%u%s",raw[i],(i+1<count)?",":"\r\n");
            furi_hal_uart_transmit(state.use_usb ? uart_usb : uart_hw, (uint8_t*)buf, l, FuriHalWaitForever);
        }
        furi_delay_ms(500);
    }

    // Cleanup (never reached in current loop)
    furi_hal_uart_deinit(uart_hw);
    furi_hal_uart_deinit(uart_usb);
    furi_hal_i2c_release(&FuriHalI2cHandleI2c);
    furi_hal_i2c_deinit_early();
    view_free(view);

    return 0;
}
