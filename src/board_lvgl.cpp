#include "board_lvgl.h"
#include <lvgl.h>
#include <chipguy_JC8012P4A1C_display.h>
#include <esp_lcd_panel_dpi_bb.h>
#include <esp_cache.h>
#include <cstring>

static chipguy_JC8012P4A1C_display g_display;
static uint8_t *g_draw_buf = nullptr;

// The driver initially scans framebuffer 0.  Start by drawing into framebuffer 1
// so we never rewrite the buffer that is currently on screen.
static uint8_t g_target_fb = 1;
static uint32_t g_last_diag_ms = 0;
static volatile bool g_touch_activity = false;
static volatile bool g_display_awake = true;
static volatile bool g_touch_pressed = false;
static volatile int16_t g_touch_x = 0;
static volatile int16_t g_touch_y = 0;
static volatile bool g_touch_position_valid = false;
static bool g_swallow_touch_until_release = false;
static uint32_t g_last_underrun_count = 0;

// Native panel geometry.  LVGL renders 1280x800 in landscape; the JD9365
// scans its framebuffer natively as 800x1280 portrait.
static constexpr int NATIVE_W = 800;
static constexpr int NATIVE_H = 1280;
static constexpr size_t NATIVE_FB_BYTES =
    static_cast<size_t>(NATIVE_W) * NATIVE_H * sizeof(uint16_t);

static uint32_t tick_cb() {
    return millis();
}

// Pixel-perfect framebuffer transform.  No scaling or interpolation is used.
static void rotate_exact(const uint16_t *src, uint16_t *dst, uint8_t rotation) {
    if (!src || !dst) return;

    switch (rotation) {
        case 0: {
            std::memcpy(dst, src, NATIVE_FB_BYTES);
            break;
        }

        // 1280x800 landscape -> 800x1280 native, 90 degrees CCW.
        case 1: {
            constexpr int SRC_W = 1280;
            constexpr int SRC_H = 800;
            constexpr int TILE = 16;

            for (int sy0 = 0; sy0 < SRC_H; sy0 += TILE) {
                const int sy_end = (sy0 + TILE < SRC_H) ? sy0 + TILE : SRC_H;
                for (int sx0 = 0; sx0 < SRC_W; sx0 += TILE) {
                    const int sx_end = (sx0 + TILE < SRC_W) ? sx0 + TILE : SRC_W;
                    for (int sy = sy0; sy < sy_end; ++sy) {
                        const uint16_t *s = src + static_cast<size_t>(sy) * SRC_W + sx0;
                        for (int sx = sx0; sx < sx_end; ++sx, ++s) {
                            const int dx = sy;
                            const int dy = (SRC_W - 1) - sx;
                            dst[static_cast<size_t>(dy) * NATIVE_W + dx] = *s;
                        }
                    }
                }
            }
            break;
        }

        case 2: {
            const size_t n = static_cast<size_t>(NATIVE_W) * NATIVE_H;
            for (size_t i = 0; i < n; ++i) {
                dst[n - 1 - i] = src[i];
            }
            break;
        }

        // 1280x800 landscape -> 800x1280 native, 270 degrees CCW / 90 CW.
        case 3: {
            constexpr int SRC_W = 1280;
            constexpr int SRC_H = 800;
            constexpr int TILE = 16;

            for (int sy0 = 0; sy0 < SRC_H; sy0 += TILE) {
                const int sy_end = (sy0 + TILE < SRC_H) ? sy0 + TILE : SRC_H;
                for (int sx0 = 0; sx0 < SRC_W; sx0 += TILE) {
                    const int sx_end = (sx0 + TILE < SRC_W) ? sx0 + TILE : SRC_W;
                    for (int sy = sy0; sy < sy_end; ++sy) {
                        const uint16_t *s = src + static_cast<size_t>(sy) * SRC_W + sx0;
                        for (int sx = sx0; sx < sx_end; ++sx, ++s) {
                            const int dx = (SRC_H - 1) - sy;
                            const int dy = sx;
                            dst[static_cast<size_t>(dy) * NATIVE_W + dx] = *s;
                        }
                    }
                }
            }
            break;
        }
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *pixelmap) {
    if (lv_display_flush_is_last(disp)) {
        uint16_t *target = g_display.getFramebuffer(g_target_fb);
        if (target) {
            rotate_exact(
                reinterpret_cast<const uint16_t *>(pixelmap),
                target,
                g_display.rotation());

            // The LCD DMA engine reads the native framebuffer directly, so
            // write back CPU cache contents before selecting the buffer.
            const esp_err_t sync_result = esp_cache_msync(
                target,
                NATIVE_FB_BYTES,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M);

            if (sync_result != ESP_OK) {
                Serial0.printf("Framebuffer cache sync failed: %d\n", static_cast<int>(sync_result));
            }

            // Select the fully-rendered inactive framebuffer and wait for the
            // next frame boundary before reusing the other one.
            g_display.setActiveFramebuffer(g_target_fb, true);
            g_target_fb ^= 1;
        }
    }

    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;

    const bool touched = g_display.getTouchDriver().getTouch(&raw_x, &raw_y);
    if (!touched) {
        g_touch_pressed = false;
        if (g_swallow_touch_until_release) g_swallow_touch_until_release = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    g_touch_activity = true;

    /* When asleep, consume the entire first touch gesture.  calendar_ui_loop()
     * sees the activity flag and restores the backlight; the user then makes a
     * second deliberate tap to activate a control. */
    if (!g_display_awake || g_swallow_touch_until_release) {
        g_touch_pressed = false;
        g_swallow_touch_until_release = true;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int32_t x = 0;
    int32_t y = 0;
    if (!g_display.mapTouch(raw_x, raw_y, x, y)) {
        g_touch_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    g_touch_x = static_cast<int16_t>(x);
    g_touch_y = static_cast<int16_t>(y);
    g_touch_position_valid = true;
    g_touch_pressed = true;

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

bool board_lvgl_begin(uint16_t rotation) {
    if (!g_display.begin(rotation)) {
        Serial0.println("ERROR: display/touch initialization failed");
        return false;
    }

    // Direct rendering with two LVGL buffers requires the two buffers to be
    // explicitly kept in sync when LVGL redraws only dirty regions.  Version
    // 1.0.8 did not do that, which could leave stale rows after overlays and
    // other partial updates.  Use one persistent full-screen LVGL buffer here;
    // the display controller still uses two native framebuffers for tear-free
    // swaps.
    g_draw_buf = reinterpret_cast<uint8_t *>(g_display.getDrawBuffer(0));
    if (!g_draw_buf) {
        Serial0.println("ERROR: display draw buffer unavailable");
        return false;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(g_display.width(), g_display.height());
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(
        disp,
        g_draw_buf,
        nullptr,
        g_display.framebufferSize(),
        LV_DISPLAY_RENDER_MODE_DIRECT);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    g_last_underrun_count = chipguy_lcd_dpi_panel_get_underrun_count();

    Serial0.printf("Display ready: %dx%d, rotation=%u\n",
                   g_display.width(), g_display.height(), g_display.rotation());
    Serial0.println("Framebuffer rotation: pixel-perfect CPU copy (no PPA filtering)");
    Serial0.println("LVGL buffering: single persistent direct buffer + double native framebuffer");
    return true;
}

void board_lvgl_loop() {
    lv_timer_handler();

    // Diagnostic only: horizontal corruption can be caused by MIPI/DMA
    // underruns.  Report only when the count changes, so normal operation is
    // silent.
    const uint32_t now = millis();
    if (now - g_last_diag_ms >= 5000) {
        g_last_diag_ms = now;
        const uint32_t underruns = chipguy_lcd_dpi_panel_get_underrun_count();
        if (underruns != g_last_underrun_count) {
            Serial0.printf("WARNING: LCD DMA underruns: %lu -> %lu\n",
                           static_cast<unsigned long>(g_last_underrun_count),
                           static_cast<unsigned long>(underruns));
            g_last_underrun_count = underruns;
        }
    }
}

void board_set_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    g_display.setBacklight(percent);
}

void board_set_display_awake(bool awake, uint8_t restore_percent) {
    if (restore_percent > 100) restore_percent = 100;
    g_display_awake = awake;
    g_display.setBacklight(awake ? restore_percent : 0);
}

bool board_display_awake() {
    return g_display_awake;
}

bool board_take_touch_activity() {
    const bool active = g_touch_activity;
    g_touch_activity = false;
    return active;
}

bool board_get_touch_state(int16_t &x, int16_t &y, bool &pressed) {
    if (!g_touch_position_valid) {
        pressed = false;
        return false;
    }

    x = g_touch_x;
    y = g_touch_y;
    pressed = g_touch_pressed;
    return true;
}

int16_t board_width() {
    return g_display.width();
}

int16_t board_height() {
    return g_display.height();
}
