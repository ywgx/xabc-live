// main/app_diag.c -- keeps what the task watchdog saw across the reset it causes.
//
// Without a serial host there is no log to read after a watchdog reboot (for example on
// battery). The watchdog ISR hook copies the stuck and running task names into RTC
// memory that survives a software reset; the next boot stores it in NVS (`config`).
#include "app.h"

#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define DIAG_MAGIC 0x57445447u   // "WDTG"

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR char s_info[120];

static void IRAM_ATTR put(const char *msg)
{
    size_t n = strnlen(s_info, sizeof(s_info));
    while (*msg && n + 1 < sizeof(s_info)) {
        const char c = *msg++;
        s_info[n++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    s_info[n] = '\0';
}

static void on_line(void *opaque, const char *msg)
{
    (void)opaque;
    put(msg);
}

void esp_task_wdt_isr_user_handler(void)
{
    s_info[0] = '\0';
    put("running=");
    put(pcTaskGetName(xTaskGetCurrentTaskHandleForCore(0)));
    put(" stuck:");
    esp_task_wdt_print_triggered_tasks(on_line, NULL, NULL);
    s_magic = DIAG_MAGIC;
}

bool app_diag_take(char *out, size_t cap)
{
    if (s_magic != DIAG_MAGIC) return false;
    s_magic = 0;
    s_info[sizeof(s_info) - 1] = '\0';
    strlcpy(out, s_info, cap);
    return true;
}
