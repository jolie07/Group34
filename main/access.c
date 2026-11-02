#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "access.h"
#include "rtc_clock.h"
#include "settings.h"
#include "util.h"

static const char *TAG = "ACCESS";

typedef struct { bool used; char uid[UID_LEN]; char name[NAME_LEN]; bool inside; } user_t;

static user_t users[MAX_USERS];
static SemaphoreHandle_t users_mutex;
static volatile bool users_dirty;

static const char *DEC_TEXT[] = {
    "ALLOW", "DENY_UNKNOWN_TAG", "DENY_ALREADY_INSIDE", "DENY_NOT_INSIDE",
    "DENY_OUTSIDE_HOURS", "DENY_SYSTEM_BUSY"
};

const char *access_decision_text(decision_t d)
{
    return ((unsigned)d < sizeof DEC_TEXT / sizeof DEC_TEXT[0]) ? DEC_TEXT[d] : "?";
}

/* ---------- persistence ---------- */
static void users_load(void)
{
    bool loaded = false;
    nvs_handle_t h;
    memset(users, 0, sizeof users);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof users;
        if (nvs_get_blob(h, "users", users, &len) == ESP_OK && len == sizeof users) loaded = true;
        nvs_close(h);
    }
    if (!loaded) {                   /* first boot: two demo users */
        memset(users, 0, sizeof users);
        users[0] = (user_t){ .used = true, .uid = "AA BB CC DD", .name = "Alice" };
        users[1] = (user_t){ .used = true, .uid = "11 22 33 44", .name = "Bob" };
        users_dirty = true;
    }
}

void access_save_now(void)
{
    user_t tmp[MAX_USERS];
    xSemaphoreTake(users_mutex, portMAX_DELAY);
    memcpy(tmp, users, sizeof tmp);
    users_dirty = false;
    xSemaphoreGive(users_mutex);
    nvs_handle_t h;                  /* the flash write happens OUTSIDE the mutex (it is slow) */
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "users", tmp, sizeof tmp);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void housekeeping_task(void *arg)     /* saves users to flash a moment after they change */
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (users_dirty) access_save_now();
    }
}

void access_init(void)
{
    users_mutex = xSemaphoreCreateMutex();
    users_load();
    xTaskCreate(housekeeping_task, "house", 4096, NULL, 2, NULL);
}

/* ---------- door hours ---------- */
bool access_schedule_open(void)
{
    static bool warned;
    uint16_t minute;
    if (!clock_minutes_now(&minute)) {
        if (!warned) { warned = true; ESP_LOGW(TAG, "Clock not set: door hours NOT enforced until settime"); }
        return true;
    }
    settings_t c = settings_get();
    return util_window_contains(c.sched_start, c.sched_end, minute);
}

/* ---------- users ---------- */
static int find_user(const char *uid)        /* caller must hold users_mutex */
{
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].used && strcmp(users[i].uid, uid) == 0) return i;
    return -1;
}

/* Check-then-change must be ONE uninterrupted step, hence the mutex. */
decision_t access_decide(reader_t reader, const char *uid, char *name_out, size_t name_n)
{
    name_out[0] = '\0';
    bool schedule_open = access_schedule_open();
    if (xSemaphoreTake(users_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return DEC_BUSY;
    decision_t d = DEC_ALLOW;
    int i = find_user(uid);
    if (i < 0) {
        d = schedule_open ? DEC_UNKNOWN : DEC_OUTSIDE_HOURS;
    } else {
        util_copy_str(name_out, name_n, users[i].name);
        if (!schedule_open)                                  d = DEC_OUTSIDE_HOURS;   /* both doors locked outside hours */
        else if (reader == READER_ENTRY && users[i].inside)  d = DEC_ALREADY_INSIDE;   /* anti-passback */
        else if (reader == READER_EXIT && !users[i].inside)  d = DEC_NOT_INSIDE;
        else { users[i].inside = (reader == READER_ENTRY); users_dirty = true; }
    }
    xSemaphoreGive(users_mutex);
    return d;
}

void access_revert(const char *uid, reader_t reader)
{
    xSemaphoreTake(users_mutex, portMAX_DELAY);
    int i = find_user(uid);
    if (i >= 0) { users[i].inside = (reader != READER_ENTRY); users_dirty = true; }
    xSemaphoreGive(users_mutex);
}

int access_count_inside(void)
{
    int c = 0;
    if (xSemaphoreTake(users_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    for (int i = 0; i < MAX_USERS; i++) if (users[i].used && users[i].inside) c++;
    xSemaphoreGive(users_mutex);
    return c;
}

void access_print_users(bool only_inside)
{
    if (xSemaphoreTake(users_mutex, pdMS_TO_TICKS(500)) != pdTRUE) { printf("busy\n"); return; }
    int shown = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].used || (only_inside && !users[i].inside)) continue;
        printf("  %-12s %s  %s\n", users[i].name, users[i].uid, users[i].inside ? "INSIDE" : "outside");
        shown++;
    }
    xSemaphoreGive(users_mutex);
    if (!shown) printf("  (none)\n");
}

user_add_result_t access_add_user(const char *uid, const char *name)
{
    user_add_result_t result = USER_TABLE_FULL;
    xSemaphoreTake(users_mutex, portMAX_DELAY);
    if (find_user(uid) >= 0) {
        result = USER_DUPLICATE;
    } else {
        for (int i = 0; i < MAX_USERS; i++) {
            if (users[i].used) continue;
            memset(&users[i], 0, sizeof users[i]);
            users[i].used = true;
            util_copy_str(users[i].uid, sizeof users[i].uid, uid);
            util_copy_str(users[i].name, sizeof users[i].name, name);
            users_dirty = true;
            result = USER_ADDED;
            break;
        }
    }
    xSemaphoreGive(users_mutex);
    return result;
}

bool access_remove_user(const char *uid)
{
    xSemaphoreTake(users_mutex, portMAX_DELAY);
    int i = find_user(uid);
    if (i >= 0) { memset(&users[i], 0, sizeof users[i]); users_dirty = true; }
    xSemaphoreGive(users_mutex);
    return i >= 0;
}
