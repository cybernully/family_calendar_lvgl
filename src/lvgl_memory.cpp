#include <lvgl.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr const char *TAG = "FamilyCalendar";
constexpr uint32_t LVGL_PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t LVGL_INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void log_allocation_failure(const char *operation, size_t size) {
    ESP_LOGE(TAG,
             "[LVGL memory] %s failed for %u bytes; PSRAM free=%u largest=%u, internal free=%u largest=%u",
             operation,
             static_cast<unsigned>(size),
             static_cast<unsigned>(heap_caps_get_free_size(LVGL_PSRAM_CAPS)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(LVGL_PSRAM_CAPS)),
             static_cast<unsigned>(heap_caps_get_free_size(LVGL_INTERNAL_CAPS)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(LVGL_INTERNAL_CAPS)));
}

} // namespace

/*
 * LVGL 9.3 defaults to LV_STDLIB_BUILTIN when LV_CONF_SKIP is used.  That
 * allocator owns a fixed 64 KB pool, which was sufficient for the original
 * one-dashboard-at-a-time UI but is too small for the v2 persistent-page
 * architecture.  ESPHome solves the same problem on ESP32 by selecting
 * LV_STDLIB_CUSTOM and preferring MALLOC_CAP_SPIRAM allocations.
 *
 * Keep object metadata, label strings, style storage, and transient LVGL
 * allocations in PSRAM whenever possible.  Internal 8-bit RAM is retained as
 * a fallback for resilience.  The display driver continues to own its DMA /
 * framebuffer allocations and is unaffected by this allocator.
 */
extern "C" {

void lv_mem_init(void) {
    // ESP-IDF/Arduino owns the heaps; nothing to initialize here.
}

void lv_mem_deinit(void) {
    // Individual LVGL allocations are released through lv_free_core().
}

void *lv_malloc_core(size_t size) {
    void *ptr = heap_caps_malloc(size, LVGL_PSRAM_CAPS);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!ptr) log_allocation_failure("malloc", size);
    return ptr;
}

void lv_free_core(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

void *lv_realloc_core(void *ptr, size_t size) {
    if (!ptr) return lv_malloc_core(size);

    void *resized = heap_caps_realloc(ptr, size, LVGL_PSRAM_CAPS);
    if (!resized) {
        /* heap_caps_realloc leaves the original allocation untouched on
         * failure, so it is safe to retry with any 8-bit capable heap. */
        resized = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    if (!resized) log_allocation_failure("realloc", size);
    return resized;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
    if (!mon) return;
    std::memset(mon, 0, sizeof(*mon));

    const size_t psram_total = heap_caps_get_total_size(LVGL_PSRAM_CAPS);
    const size_t psram_free = heap_caps_get_free_size(LVGL_PSRAM_CAPS);
    const size_t psram_largest = heap_caps_get_largest_free_block(LVGL_PSRAM_CAPS);

    const size_t internal_total = heap_caps_get_total_size(LVGL_INTERNAL_CAPS);
    const size_t internal_free = heap_caps_get_free_size(LVGL_INTERNAL_CAPS);
    const size_t internal_largest = heap_caps_get_largest_free_block(LVGL_INTERNAL_CAPS);

    mon->total_size = psram_total + internal_total;
    mon->free_size = psram_free + internal_free;
    mon->free_biggest_size = std::max(psram_largest, internal_largest);

    if (mon->total_size > 0) {
        const size_t used = mon->total_size - mon->free_size;
        mon->used_pct = static_cast<uint8_t>((used * 100U) / mon->total_size);
    }

    if (mon->free_size > 0) {
        const size_t biggest_pct = (mon->free_biggest_size * 100U) / mon->free_size;
        mon->frag_pct = static_cast<uint8_t>(biggest_pct >= 100U ? 0U : 100U - biggest_pct);
    }
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

} // extern "C"
