
#include <stddef.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_usb.h>
#include <infrared.h>
#include <infrared_worker.h>
#include <storage/storage.h>
#include <toolbox/name_generator.h>

#define TAG        "BGW_flipper_tunnel"
#define MAX_BUF    64
#define HOLD_TIME_MS 1000

typedef struct {
    bool use_usb;
    bool use_uart;
    bool use_i2c;
    bool log_to_file;
    uint32_t last_rng;
    FuriMutex* mutex;
    bool in_menu;
    uint32_t back_pressed_time;
    uint32_t ok_pressed_time;
} FlameTunnelState;

static void process_ir(FlameTunnelState* s, uint32_t rng) {
    char buf[MAX_BUF];
    int len = snprintf(buf, MAX_BUF, "RNG:%lu\n", rng);

    if(s->use_usb) furi_hal_usb_tx((uint8_t*)buf, len, FuriHalWaitForever);
    if(s->use_uart) furi_hal_serial_tx((uint8_t*)buf, len, FuriHalWaitForever);
    if(s->use_i2c) {
        // Add your I2C transmission logic here
    }

    if(s->log_to_file) {
        Storage* st = furi_record_open(RECORD_STORAGE);
        Stream* file = file_stream_alloc(st);
        file_stream_open(file, "/ext/flame_tunnel.log", FSAM_APPEND, FSOM_OPEN_EXISTING);
        stream_write(file, (uint8_t*)buf, len);
        file_stream_close(file);
        file_stream_free(file);
        furi_record_close(RECORD_STORAGE);
    }

    s->last_rng = rng;
}

static uint32_t generate_rng(IRWorkerSignal* sig) {
    const uint32_t* times; size_t n;
    infrared_worker_get_raw_signal(sig, &times, &n);
    uint32_t seed = furi_hal_random_get() ^ DWT->CYCCNT;
    for(size_t i = 0; i < n; i++) seed ^= times[i] + i;
    return seed;
}

static void ir_callback(void* ctx, InfraredWorkerSignal* sig) {
    FlameTunnelState* s = ctx;
    uint32_t rng = generate_rng(sig);
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    process_ir(s, rng);
    furi_mutex_release(s->mutex);
}

static void draw(Canvas* c, void* ctx) {
    FlameTunnelState* s = ctx;
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    if(s->in_menu) {
        canvas_draw_str(c, 2, 10, "Config Menu");
        int y = 30;
        #define OPTION(name, flag) canvas_draw_str(c, 2, y, flag ? name " ON" : name " OFF"); y+=10;
        OPTION("USB", s->use_usb);
        OPTION("UART", s->use_uart);
        OPTION("I2C", s->use_i2c);
        OPTION("Log", s->log_to_file);
    } else {
        canvas_draw_str(c, 2, 10, "Flame Tunnel");
        char buf[32];
        snprintf(buf, sizeof(buf), "%06lu", s->last_rng);
        canvas_set_font(c, FontBigNumbers);
        canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignCenter, buf);
        canvas_set_font(c, FontSecondary);
        int y = 60;
        #define STATUS(name, flag) canvas_draw_str(c, 2, y, flag ? name " ON" : name " OFF"); y+=10;
        STATUS("USB", s->use_usb);
        STATUS("UART", s->use_uart);
        STATUS("I2C", s->use_i2c);
        STATUS("Log", s->log_to_file);
    }
}

static void input_cb(InputEvent* ev, void* ctx) {
    FlameTunnelState* s = ctx;
    if(ev->type == InputTypePress) {
        if(ev->key == InputKeyBack) s->back_pressed_time = furi_get_tick();
        if(ev->key == InputKeyOk) s->ok_pressed_time = furi_get_tick();
    } else if(ev->type == InputTypeRelease) {
        uint32_t now = furi_get_tick();
        if(ev->key == InputKeyBack && now - s->back_pressed_time >= HOLD_TIME_MS) {
            furi_exit_app();
        } else if(ev->key == InputKeyOk && now - s->ok_pressed_time >= HOLD_TIME_MS) {
            s->in_menu = !s->in_menu;
        } else if(s->in_menu) {
            switch(ev->key) {
                case InputKeyLeft: s->use_usb = !s->use_usb; break;
                case InputKeyRight: s->use_uart = !s->use_uart; break;
                case InputKeyUp: s->use_i2c = !s->use_i2c; break;
                case InputKeyOk: s->log_to_file = !s->log_to_file; break;
                default: break;
            }
        }
    }
}

int32_t bgw_flipper_tunnel(void* p) {
    UNUSED(p);
    FlameTunnelState st = {
        .use_usb = true,
        .use_uart = false,
        .use_i2c = false,
        .log_to_file = false,
        .last_rng = 0,
        .mutex = furi_mutex_alloc(FuriMutexTypeNormal),
        .in_menu = false,
        .back_pressed_time = 0,
        .ok_pressed_time = 0,
    };

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw, &st);
    view_port_input_callback_set(vp, input_cb, &st);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InfraredWorker* w = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(w, ir_callback, &st);
    infrared_worker_rx_start(w);

    while(1) {
        view_port_update(vp);
        furi_delay_ms(100);
    }

    infrared_worker_rx_stop(w);
    infrared_worker_free(w);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_mutex_free(st.mutex);
    furi_record_close(RECORD_GUI);
    return 0;
}

