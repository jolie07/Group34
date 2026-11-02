#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <stdio.h>
#include "app_config.h"
#include "rooms.h"
#include "event_log.h"
#include "settings.h"

static room_t rooms[2] = {
    { "Lecture Room 1", PIR1_PIN, RELAY1_PIN, BTN1_PIN, false, false, false, true, -1000000, 0, 0, 0 },
    { "Lecture Room 2", PIR2_PIN, RELAY2_PIN, BTN2_PIN, false, false, false, true, -1000000, 0, 0, 0 },
};
static SemaphoreHandle_t rooms_mutex;

room_t *rooms_get(int index) { return &rooms[index]; }

/* One update step for one room. */
void room_update(room_t *r, int64_t now, int motion, int btn, int64_t timeout_ms, bool *toggled, bool *changed)
{
    *toggled = false;
    *changed = false;
    if (motion) r->last_motion_ms = now;
    r->occupied = motion || (now - r->last_motion_ms) < timeout_ms;
    if (btn == 0 && r->btn_prev && (now - r->btn_ms) > 250) {      /* button pressed (debounced) */
        r->hold = !r->hold;
        r->btn_ms = now;
        *toggled = true;
    }
    r->btn_prev = (btn != 0);         /*recall button state*/
    bool want = r->hold || r->occupied;
    if (want != r->light_on) {
        r->light_on = want;
        gpio_set_level(r->relay, want ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
        if (want) r->on_since_ms = now; else r->on_total_ms += now - r->on_since_ms;
        *changed = true;
    }
}

static void rooms_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t timeout_ms = (int64_t)settings_get().light_timeout_s * 1000;
        for (int i = 0; i < 2; i++) {
            room_t *r = &rooms[i];
            int motion = gpio_get_level(r->pir);
            int btn = gpio_get_level(r->btn);
            bool toggled, changed;
            if (xSemaphoreTake(rooms_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
            room_update(r, now, motion, btn, timeout_ms, &toggled, &changed);
            bool hold = r->hold, light = r->light_on;
            xSemaphoreGive(rooms_mutex);
            if (toggled) log_event("ROOM", "", r->name, hold ? "HOLD_ON" : "HOLD_OFF");
            if (changed) {
                log_event("LIGHT", "", r->name, light ? "ON" : "OFF");
                if (light) {
                    printf("[%s] %s - light ON\n", r->name,
                           motion ? "Motion detected" : "Manual hold");
                } else {
                    printf("[%s] No motion timeout - light OFF\n", r->name);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void rooms_init(void)
{
    rooms_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(rooms[i].pir);
        gpio_set_direction(rooms[i].pir, GPIO_MODE_INPUT);
        gpio_reset_pin(rooms[i].relay);
        gpio_set_direction(rooms[i].relay, GPIO_MODE_OUTPUT);
        gpio_set_level(rooms[i].relay, !RELAY_ACTIVE_LEVEL);
        gpio_reset_pin(rooms[i].btn);
        gpio_set_direction(rooms[i].btn, GPIO_MODE_INPUT);
        gpio_set_pull_mode(rooms[i].btn, GPIO_PULLUP_ONLY);
    }
    xTaskCreate(rooms_task, "rooms", 4096, NULL, 4, NULL);
}

void rooms_print(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (xSemaphoreTake(rooms_mutex, pdMS_TO_TICKS(500)) != pdTRUE) { printf("busy\n"); return; }
    for (int i = 0; i < 2; i++) {
        room_t *r = &rooms[i];
        int64_t on = r->on_total_ms + (r->light_on ? now - r->on_since_ms : 0);
        printf("  %s: %s | light %s%s | on %d s of %d s\n",
               r->name, r->occupied ? "OCCUPIED" : "empty", r->light_on ? "ON" : "off",
               r->hold ? " (HOLD)" : "", (int)(on / 1000), (int)(now / 1000));
    }
    xSemaphoreGive(rooms_mutex);
}
