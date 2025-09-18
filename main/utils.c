// special_day.c

#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_sntp.h"

#define TAG "SPECIAL_DAY"

// Define birthday
#define BIRTHDAY_MONTH 6   // June
#define BIRTHDAY_DAY   28

typedef struct {
    int month;
    int day;
    const char *name;
} holiday_t;

// List of fixed Swedish public holidays
static const holiday_t sweden_holidays[] = {
    {1, 1,   "New Year's Day"},
    {1, 6,   "Epiphany"},
    {5, 1,   "Labour Day"},
    {6, 6,   "National Day"},
    {12, 24, "Christmas Eve"},
    {12, 25, "Christmas Day"},
    {12, 26, "Boxing Day"},
    {12, 31, "New Year's Eve"},
};

static bool s_sntp_initialized = false; // Add this flag

// SNTP initialization
void special_day_init_sntp(void)
{
    if (s_sntp_initialized) {
        ESP_LOGI(TAG, "SNTP already initialized. Skipping.");
        return; // Exit if already initialized
    }
    
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    s_sntp_initialized = true; // Set flag after successful initialization

    // Wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2022 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2022 - 1900)) {
        ESP_LOGI(TAG, "System time is now synced.");
    } else {
        ESP_LOGW(TAG, "Failed to sync time via SNTP.");
    }

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", time_str);
}

bool is_birthday(void) {
    time_t now;
    struct tm timeinfo;

    if (!s_sntp_initialized) {
        ESP_LOGE(TAG, "SNTP not initialized. Call special_day_init_sntp() first.");
        return false; // SNTP not initialized
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_mon != BIRTHDAY_MONTH - 1) {
        ESP_LOGI(TAG, "Not the birthday month: %d", timeinfo.tm_mon + 1);
        return false; // Not the birthday month
    }

    if (timeinfo.tm_mday == BIRTHDAY_DAY) {
        ESP_LOGI(TAG, "the birthday day: %d", timeinfo.tm_mday);
        return true; // Not the birthday day
    }

    if (timeinfo.tm_mday == BIRTHDAY_DAY - 1) {
        ESP_LOGI(TAG, "Before the birthday day: %d", timeinfo.tm_mday);
        return true; // Before the birthday day
    }

    return false; // Not the birthday day or before
}

// const char *get_special_day_name(struct tm *timeinfo) {
//     if (is_birthday()) {
//         return "Birthday";
//     }

//     for (int i = 0; i < sizeof(sweden_holidays) / sizeof(sweden_holidays[0]); i++) {
//         if ((sweden_holidays[i].month == timeinfo->tm_mon + 1) &&
//             (sweden_holidays[i].day == timeinfo->tm_mday)) {
//             return sweden_holidays[i].name;
//         }
//     }

//     return NULL;
// }

bool is_special_day(void) {
    time_t now;
    struct tm timeinfo;

    if (!s_sntp_initialized) {
        ESP_LOGE(TAG, "SNTP not initialized. Call special_day_init_sntp() first.");
        return false; // SNTP not initialized
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < sizeof(sweden_holidays) / sizeof(sweden_holidays[0]); i++) {
        if (sweden_holidays[i].month != timeinfo.tm_mon + 1) {
            continue; // Not the same month
        }
        if (sweden_holidays[i].day == timeinfo.tm_mday) {
            return true;
        }
        else if (sweden_holidays[i].day == timeinfo.tm_mday - 1) {
            ESP_LOGI(TAG, "Special day is tomorrow: %s", sweden_holidays[i].name);
            return true; // Special day is tomorrow
        }
    }

    return false;

}

// Helper function to convert HSV to RGB
void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = v - c;
    float r_temp, g_temp, b_temp;

    if (h < 60)
    {
        r_temp = c;
        g_temp = x;
        b_temp = 0;
    }
    else if (h < 120)
    {
        r_temp = x;
        g_temp = c;
        b_temp = 0;
    }
    else if (h < 180)
    {
        r_temp = 0;
        g_temp = c;
        b_temp = x;
    }
    else if (h < 240)
    {
        r_temp = 0;
        g_temp = x;
        b_temp = c;
    }
    else if (h < 300)
    {
        r_temp = x;
        g_temp = 0;
        b_temp = c;
    }
    else
    {
        r_temp = c;
        g_temp = 0;
        b_temp = x;
    }

    *r = (uint8_t)((r_temp + m) * 255);
    *g = (uint8_t)((g_temp + m) * 255);
    *b = (uint8_t)((b_temp + m) * 255);
}