// #if 0
#include <furi.h>
#include <stdbool.h>
#include <gui/gui.h>
#include <furi_hal.h>
#include <furi_hal_usb_i.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <infrared.h>
#include <infrared_worker.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/stream/stream.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <toolbox/api_lock.h>
#include "furi_hal_serial_types.h"
#include "usb_cdc.h"

#define USB_CDC_BIT_DTR        (1 << 0)
#define USB_CDC_BIT_RTS        (1 << 1)
#define MAX_BUF                64
#define HOLD_TIME_MS           1000
#define USB_CDC_PKT_LEN        CDC_DATA_SZ
#define USB_UART_RX_BUF_SIZE   (USB_CDC_PKT_LEN * 5)

typedef struct UsbUart UsbUart;
typedef enum {
    WorkerEvtStop      = (1 << 0),
    WorkerEvtCdcRx     = (1 << 1),
    WorkerEvtCfgChange = (1 << 2),
} WorkerEvtFlags;
#define WORKER_ALL_EVENTS (WorkerEvtStop | WorkerEvtCfgChange | WorkerEvtCdcRx)

typedef struct {
    FuriHalSerialHandle* serial_handle;
    bool                running;
    uint32_t            back_pressed_time;
    uint32_t            ok_pressed_time;
    bool                use_usb;
    bool                use_uart;
    bool                use_i2c;
    bool                log_to_file;
    uint32_t            last_rng;
    FuriMutex*          mutex;
    bool                in_menu;
} FlameTunnelState;

typedef struct {
    uint8_t                     vcp_ch;
    size_t (*rx_data)(void* ctx, uint8_t* data, size_t length);
    void*                       rx_data_ctx;
} UsbUartConfig;

typedef struct {
    uint32_t rx_cnt;
    uint32_t tx_cnt;
} UsbUartState;

/* Forward declarations for VCP callback functions */
static void vcp_on_cdc_tx_complete(void* context);
static void vcp_on_cdc_rx(void* context);
static void vcp_state_callback(void* context, uint8_t state);
static void vcp_on_cdc_control_line(void* context, uint8_t state);
static void vcp_on_line_config(void* context, struct usb_cdc_line_coding* config);

/* CDC callbacks configuration */
static CdcCallbacks cdc_cb = {
    .tx_ep_callback      = &vcp_on_cdc_tx_complete,
    .rx_ep_callback      = &vcp_on_cdc_rx,
    .state_callback      = &vcp_state_callback,
    .ctrl_line_callback  = &vcp_on_cdc_control_line,
    .config_callback     = &vcp_on_line_config
};

/* USB↔UART bridge struct */
struct UsbUart {
    UsbUartConfig  cfg;
    UsbUartConfig  cfg_new;
    FuriThread*    thread;
    FuriMutex*     usb_mutex;
    FuriSemaphore* tx_sem;
    UsbUartState   st;
    FuriApiLock    cfg_lock;
    uint8_t        rx_buf[USB_CDC_PKT_LEN];
};

/* Prototypes */
UsbUart* usb_uart_enable(UsbUartConfig* cfg);
void     usb_uart_disable(UsbUart* usb_uart);
void     usb_uart_set_config(UsbUart* usb_uart, UsbUartConfig* cfg);
void     usb_uart_get_config(UsbUart* usb_uart, UsbUartConfig* cfg);
void     usb_uart_get_state(UsbUart* usb_uart, UsbUartState* st);
void     usb_uart_tx_data(UsbUart* usb_uart, uint8_t* data, size_t length);

/* Generate RNG from IR signal */
static uint32_t generate_rng(InfraredWorkerSignal* sig) {
    const uint32_t* times;
    size_t n;
    infrared_worker_get_raw_signal(sig, &times, &n);
    uint32_t seed = furi_hal_random_get() ^ DWT->CYCCNT;
    for(size_t i = 0; i < n; i++) seed ^= times[i] + i;
    return seed;
}

/* Initialize USB CDC VCP channel */
static void usb_uart_vcp_init(UsbUart* usb_uart, uint8_t vcp_ch) {
    furi_hal_usb_unlock();
    if(vcp_ch == 0) {
        furi_check(furi_hal_usb_set_config(&usb_cdc_single, NULL) == true);
    } else {
        furi_check(furi_hal_usb_set_config(&usb_cdc_dual , NULL) == true);
    }
    furi_hal_cdc_set_callbacks(vcp_ch, &cdc_cb, usb_uart);
}

/* Deinitialize USB CDC VCP channel */
static void usb_uart_vcp_deinit(UsbUart* usb_uart, uint8_t vcp_ch) {
    UNUSED(usb_uart);
    furi_hal_cdc_set_callbacks(vcp_ch, NULL, NULL);
}

/* Transmit data over CDC */
void usb_uart_tx_data(UsbUart* usb_uart, uint8_t* data, size_t length) {
    uint32_t pos = 0;
    while(pos < length) {
        size_t pkt_size = length - pos;
        if(pkt_size > USB_CDC_PKT_LEN) pkt_size = USB_CDC_PKT_LEN;
        if(furi_semaphore_acquire(usb_uart->tx_sem, 100) == FuriStatusOk) {
            furi_check(furi_mutex_acquire(usb_uart->usb_mutex, FuriWaitForever) == FuriStatusOk);
            furi_hal_cdc_send(usb_uart->cfg.vcp_ch, &data[pos], pkt_size);
            furi_check(furi_mutex_release(usb_uart->usb_mutex) == FuriStatusOk);
            usb_uart->st.tx_cnt += pkt_size;
            pos += pkt_size;
        }
    }
}

/* Worker thread handling RX and config events */
static int32_t usb_uart_worker(void* context) {
    UsbUart* usb_uart = context;
    memcpy(&usb_uart->cfg, &usb_uart->cfg_new, sizeof(UsbUartConfig));
    usb_uart->tx_sem   = furi_semaphore_alloc(1,1);
    usb_uart->usb_mutex= furi_mutex_alloc(FuriMutexTypeNormal);
    usb_uart_vcp_init(usb_uart, usb_uart->cfg.vcp_ch);

    uint8_t data[2*USB_CDC_PKT_LEN];
    size_t  remain = 0;

    while(1) {
        uint32_t events = furi_thread_flags_wait(
            WORKER_ALL_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        furi_check(!(events & FuriFlagError));
        if(events & WorkerEvtStop) break;

        if(events & WorkerEvtCdcRx) {
            furi_check(furi_mutex_acquire(usb_uart->usb_mutex, FuriWaitForever)==FuriStatusOk);
            size_t len = furi_hal_cdc_receive(
                usb_uart->cfg.vcp_ch, &data[remain], USB_CDC_PKT_LEN);
            furi_check(furi_mutex_release(usb_uart->usb_mutex)==FuriStatusOk);
            if(len>0) {
                usb_uart->st.rx_cnt += len;
                remain += len;
                size_t handled = usb_uart->cfg.rx_data(
                    usb_uart->cfg.rx_data_ctx, data, remain);
                memmove(data, &data[handled], remain-handled);
                remain -= handled;
            }
        }
        if(events & WorkerEvtCfgChange) {
            if(usb_uart->cfg.vcp_ch != usb_uart->cfg_new.vcp_ch) {
                usb_uart_vcp_deinit(usb_uart, usb_uart->cfg.vcp_ch);
                usb_uart_vcp_init(usb_uart, usb_uart->cfg_new.vcp_ch);
                usb_uart->cfg.vcp_ch = usb_uart->cfg_new.vcp_ch;
            }
            api_lock_unlock(usb_uart->cfg_lock);
        }
    }

    usb_uart_vcp_deinit(usb_uart, usb_uart->cfg.vcp_ch);
    furi_mutex_free(usb_uart->usb_mutex);
    furi_semaphore_free(usb_uart->tx_sem);
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_cdc_single, NULL)==true);
    return 0;
}

/* VCP callbacks */
static void vcp_on_cdc_tx_complete(void* context) {
    (void)context;
}
static void vcp_on_cdc_rx(void* context) {
    UsbUart* usb_uart = context;
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtCdcRx);
}
static void vcp_state_callback(void* context, uint8_t state) {
    (void)context; (void)state;
}
static void vcp_on_cdc_control_line(void* context, uint8_t state) {
    (void)context; (void)state;
}
static void vcp_on_line_config(void* context, struct usb_cdc_line_coding* config) {
    (void)context; (void)config;
}

/* Public USB↔UART API */
UsbUart* usb_uart_enable(UsbUartConfig* cfg) {
    UsbUart* usb_uart = malloc(sizeof(UsbUart));
    memcpy(&usb_uart->cfg_new, cfg, sizeof(UsbUartConfig));
    usb_uart->thread = furi_thread_alloc_ex(
        "UsbUartWorker", 1024, usb_uart_worker, usb_uart);
    furi_thread_start(usb_uart->thread);
    return usb_uart;
}
void usb_uart_disable(UsbUart* usb_uart) {
    furi_assert(usb_uart);
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtStop);
    furi_thread_join(usb_uart->thread);
    furi_thread_free(usb_uart->thread);
    free(usb_uart);
}
void usb_uart_set_config(UsbUart* usb_uart, UsbUartConfig* cfg) {
    furi_assert(usb_uart && cfg);
    usb_uart->cfg_lock = api_lock_alloc_locked();
    memcpy(&usb_uart->cfg_new, cfg, sizeof(UsbUartConfig));
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtCfgChange);
    api_lock_wait_unlock_and_free(usb_uart->cfg_lock);
}
void usb_uart_get_config(UsbUart* usb_uart, UsbUartConfig* cfg) {
    furi_assert(usb_uart && cfg);
    memcpy(cfg, &usb_uart->cfg_new, sizeof(UsbUartConfig));
}
void usb_uart_get_state(UsbUart* usb_uart, UsbUartState* st) {
    furi_assert(usb_uart && st);
    memcpy(st, &usb_uart->st, sizeof(UsbUartState));
}

/* Handle an IR event */
static void process_ir(FlameTunnelState* s, uint32_t rng) {
    char buf[MAX_BUF];
    int len = snprintf(buf, MAX_BUF, "RNG:%lu\n", (unsigned long)rng);

    if(s->use_usb) {
        furi_hal_cdc_send(0, (uint8_t*)buf, len);
    }
    if(s->use_uart) {
        furi_hal_serial_tx(s->serial_handle, (const uint8_t*)buf, len);
    }
    if(s->log_to_file) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        Stream* file    = file_stream_alloc(storage);
        file_stream_open(file, "/ext/flame_tunnel.log",
                         FSAM_WRITE, FSOM_OPEN_ALWAYS);
        stream_write(file, (const uint8_t*)buf, len);
        file_stream_close(file);
        // stream_free(file);
        furi_record_close(RECORD_STORAGE);
    } s->last_rng = rng;
}

/* IR worker callback */
static void ir_callback(void* ctx, InfraredWorkerSignal* sig) {
    FlameTunnelState* s = ctx;
    uint32_t rng = generate_rng(sig);
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    process_ir(s, rng);
    furi_mutex_release(s->mutex);
}

/* Draw UI */
static void draw(Canvas* canvas, void* ctx) {
    FlameTunnelState* s = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(s->in_menu) {
        canvas_draw_str(canvas, 2, 10, "Config Menu");
        int y = 30;
        #define OPTION(name, flag) \
            canvas_draw_str(canvas, 2, y, (flag)? name " ON": name " OFF"); y += 10;
        OPTION("USB",  s->use_usb);
        OPTION("UART", s->use_uart);
        OPTION("I2C",  s->use_i2c);
        OPTION("Log",  s->log_to_file);
    } else {
    char buf[32];
    canvas_draw_str(canvas, 2, 10, "Flame Tunnel");
        snprintf(buf, sizeof(buf), "%06lu", (unsigned long)s->last_rng);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 64, 30,
                                AlignCenter, AlignCenter, buf);
        canvas_set_font(canvas, FontSecondary);
        int y = 60;
        #define STATUS(name, flag) \
            canvas_draw_str(canvas, 2, y, (flag)? name " ON": name " OFF"); y += 10;
        STATUS("USB",  s->use_usb);
        STATUS("UART", s->use_uart);
        STATUS("I2C",  s->use_i2c);
        STATUS("Log",  s->log_to_file);
    }
}

/* Input handler */
static void input_cb(InputEvent* ev, void* ctx) {
    FlameTunnelState* s = ctx;
    uint32_t now = furi_get_tick();

    if(ev->type == InputTypePress) {
        if(ev->key == InputKeyBack) s->back_pressed_time = now;
        if(ev->key == InputKeyOk  ) s->ok_pressed_time   = now;
    } else if(ev->type == InputTypeRelease) {
        if(ev->key == InputKeyBack &&
           now - s->back_pressed_time >= HOLD_TIME_MS) {
            s->running = false;
        } else if(ev->key == InputKeyOk &&
                  now - s->ok_pressed_time >= HOLD_TIME_MS) {
            s->in_menu = !s->in_menu;
        } else if(s->in_menu) {
            switch(ev->key) {
                case InputKeyLeft:  s->use_usb    = !s->use_usb;    break;
                case InputKeyRight: s->use_uart   = !s->use_uart;   break;
                case InputKeyUp:    s->use_i2c    = !s->use_i2c;    break;
                case InputKeyOk:    s->log_to_file= !s->log_to_file;break;
                default: break;
            }
        }
    }
}

/* Application entry point */
int32_t bgw_flipper_tunnel_app(void* p) {
UNUSED(p);
    FlameTunnelState st = {
        .running           = true,
        .use_usb           = true,
        .use_uart          = false,
        .use_i2c           = false,
        .log_to_file       = false,
        .last_rng          = 0,
        .mutex             = NULL,
        .in_menu           = false,
        .back_pressed_time = 0,
        .ok_pressed_time   = 0,
    };

    st.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    // Acquire serial interface
    furi_hal_serial_control_init();
    st.serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_hal_serial_init(st.serial_handle, 115200);
    
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw,      &st);
    view_port_input_callback_set(vp, input_cb, &st);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InfraredWorker* worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(
        worker, ir_callback, &st);
    infrared_worker_rx_start(worker);

    while(st.running) {
        view_port_update(vp);
        furi_delay_ms(100);
    }

    // Release serial interface
    furi_hal_serial_deinit(st.serial_handle);
    furi_hal_serial_control_release(st.serial_handle);

    infrared_worker_rx_stop(worker);
    infrared_worker_free(worker);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_mutex_free(st.mutex);
    furi_record_close(RECORD_GUI);

    return 0;
    }
// #endif