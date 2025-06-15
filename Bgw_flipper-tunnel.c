#include "furi.h"
#include "furi_hal.h"
#include "ir.h"

#define USE_HW_UART true
#define USE_USB_UART false

int32_t BGW_FLIPPER_tunnel_app(void* p) {
    UNUSED(p);

    FuriHalUartSettings settings = {
        .baud_rate = 115200,
        .data_bits = FuriHalUartDataBits8,
        .stop_bits = FuriHalUartStopBits1,
        .parity = FuriHalUartParityNone
    };

    FuriHalUart* uart_hw = NULL;
    FuriHalUart* uart_usb = NULL;

    if(USE_HW_UART) {
        uart_hw = furi_hal_uart_init(FuriHalUartIdMain, &settings);
        furi_hal_uart_transmit(uart_hw, (uint8_t*)"BGW Tunnel HW: starting...\r\n", 28, FuriHalWaitForever);
    }

    if(USE_USB_UART) {
        uart_usb = furi_hal_uart_init(FuriHalUartIdUsb, &settings);
        furi_hal_uart_transmit(uart_usb, (uint8_t*)"BGW Tunnel USB: starting...\r\n", 29, FuriHalWaitForever);
    }

    while(1) {
        uint16_t raw[128];
        size_t count = 0;
        if(ir_read_raw(raw, &count, 128, 1000) == IR_OK && count > 0) {
            for(size_t i = 0; i < count; i++) {
                char buf[16];
                int len = snprintf(buf, sizeof(buf), "%u%s", raw[i],
                    (i + 1 < count) ? "," : "\r\n");
                if(USE_HW_UART) furi_hal_uart_transmit(uart_hw, (uint8_t*)buf, len, FuriHalWaitForever);
                if(USE_USB_UART) furi_hal_uart_transmit(uart_usb, (uint8_t*)buf, len, FuriHalWaitForever);
            }
        }
        furi_delay_ms(100);
    }

    if(uart_hw) furi_hal_uart_deinit(uart_hw);
    if(uart_usb) furi_hal_uart_deinit(uart_usb);
    return 0;
}
