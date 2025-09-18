#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <esp_timer.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "lightbulb.h"
#include "driver/gpio.h"
#include <math.h>

static const char *TAG = "lightbulb";

// Default color (warm: 255, 144, 58)
#define DEFAULT_RED 255
#define DEFAULT_GREEN 144
#define DEFAULT_BLUE 58
#define DEFAULT_DURATION_HOURS 8

#define BRIGHTNESS_STEP_INTERVAL (30 * 60 * 1000) // 30 minutes in milliseconds

#define MAX_LEDS 60                               // Maximum number of LEDs

#define RMT_CLK_DIV 2                             // 80MHz / 2 = 40MHz resolution

#define RMT_LED_STRIP_RESOLUTION_HZ 40000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_TX_GPIO_PRIMARY      4

typedef struct
{
    bool is_on;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    float hue;        // 0-360 degrees
    float saturation; // 0-1
    float value;     // 0-1, brightness
    int brightness_total_steps;
    TaskHandle_t lightbulb_task_handle;
    QueueHandle_t lightbulb_task_queue;
    uint8_t rgb_data[3 * MAX_LEDS]; // GRB order for WS2812
} lightbulb_state_t;

static lightbulb_state_t bulb_state = {
    .red = DEFAULT_RED,
    .green = DEFAULT_GREEN,
    .blue = DEFAULT_BLUE,
    .hue = 26.0f,        // Approximate hue for warm color (255, 144, 58)
    .saturation = 0.77f, // Approximate saturation for warm color
    .is_on = false,
    .lightbulb_task_handle = NULL};

static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

extern void special_day_init_sntp(void);
extern void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
extern bool is_birthday(void);
extern bool is_special_day(void);

// Initialize RMT for WS2812
static esp_err_t rmt_tx_init(gpio_num_t gpio, rmt_channel_handle_t *channel, rmt_encoder_handle_t *encoder)
{
    rmt_tx_channel_config_t config = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ, // 40MHz
        .mem_block_symbols = 64,
        .trans_queue_depth = 4
    };
    esp_err_t ret = rmt_new_tx_channel(&config, channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create RMT TX channel for GPIO %d: %s", gpio, esp_err_to_name(ret));
        return ret;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ret = rmt_new_led_strip_encoder(&encoder_config, encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create WS2812 encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(*channel);
        *channel = NULL;
        return ret;
    }
    ret = rmt_enable(*channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable RMT TX channel for GPIO %d: %s", gpio, esp_err_to_name(ret));
        rmt_del_channel(*channel);
        *channel = NULL;
        return ret;
    }
    return ESP_OK;
}

static void set_rgb_color(uint8_t red, uint8_t green, uint8_t blue)
{
    bulb_state.red = red;
    bulb_state.green = green;
    bulb_state.blue = blue;

    for (int i = 0; i < MAX_LEDS; i++)
    {
        bulb_state.rgb_data[i * 3] = green;   // G
        bulb_state.rgb_data[i * 3 + 1] = red; // R
        bulb_state.rgb_data[i * 3 + 2] = blue; // B
    }
}

static void set_rainbow_color(int step)
{
    // Generate rainbow colors based on the step
    float hue = (float)(step % 360);
    float saturation = bulb_state.saturation; // Full saturation
    float value = bulb_state.value;      // Full brightness

    for (int i = 0; i < MAX_LEDS; i++)
    {
        uint8_t r, g, b;

        // rotate hue for each LED
        hue = fmodf(hue + (i * 12), 360.0f);
        hsv_to_rgb(hue + (i * 12), saturation, value, &r, &g, &b);
        bulb_state.rgb_data[i * 3] = g;   // G
        bulb_state.rgb_data[i * 3 + 1] = r; // R
        bulb_state.rgb_data[i * 3 + 2] = b; // B
    }
}

// Send RGB data to LED strip
static void send_rgb_to_strip(void)
{
    if (!tx_channel || !led_encoder)
    {
        ESP_LOGW(TAG, "RMT channel or encoder not initialized, skipping");
        return;
    }

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    esp_err_t ret = rmt_transmit(tx_channel, led_encoder, bulb_state.rgb_data, sizeof(bulb_state.rgb_data), &transmit_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = rmt_tx_wait_all_done(tx_channel, portMAX_DELAY);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "RMT wait all done failed: %s", esp_err_to_name(ret));
    }
}

static void lightbulb_task(void *pvParameters)
{
    uint32_t event = 0;
    int total_steps = bulb_state.brightness_total_steps;
    int step_duration_ms;
    int auto_step_schedule_ms = 0;
    int starting_time = 0;
    int auto_step_index = 0, auto_step_size = 0;
    bool is_rainbow_mode = false;
    int current_step_index = -1;

    bulb_state.lightbulb_task_queue = xQueueCreate(6, sizeof(int));

    while (1)
    {

        // wait and check queue if new brightness event comes in
        if (xQueueReceive(bulb_state.lightbulb_task_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            switch (event)
            {
            case LIGHTBULB_EVENT_OFF:
                set_rgb_color(0, 0, 0); // Set RGB to off
                send_rgb_to_strip();
                auto_step_index = 0;
                current_step_index = -1;
            break;
            case LIGHTBULB_EVENT_ON:
                current_step_index = -1;
                auto_step_index = 0;
                if (is_birthday() || is_special_day())
                {
                    is_rainbow_mode = true;
                    auto_step_size = 360 / 20; // 360 degrees for rainbow effect
                    step_duration_ms = 10000;
                    auto_step_schedule_ms = step_duration_ms + esp_timer_get_time() / 1000;
                    // how many 10 seconds in total
                    total_steps = (DEFAULT_DURATION_HOURS * 3600 * 1000) / 10;
                }
                else
                {
                    total_steps = bulb_state.brightness_total_steps;
                    step_duration_ms = (DEFAULT_DURATION_HOURS * 3600 * 1000) / total_steps;
                    hsv_to_rgb(bulb_state.hue, bulb_state.saturation, bulb_state.value, &bulb_state.red, &bulb_state.green, &bulb_state.blue);
                    set_rgb_color(bulb_state.red, bulb_state.green, bulb_state.blue);
                }
            break;

            case LIGHTBULB_EVENT_HUE:
                is_rainbow_mode = false; // Disable rainbow mode when hue changes
                current_step_index = -1;
                auto_step_index = 0;
                // Update RGB based on new hue
                hsv_to_rgb(bulb_state.hue, bulb_state.saturation, bulb_state.value, &bulb_state.red, &bulb_state.green, &bulb_state.blue);
                set_rgb_color(bulb_state.red, bulb_state.green, bulb_state.blue);
                break;
            case LIGHTBULB_EVENT_REFRESH:
                ESP_LOGW(TAG, " Refresh : %d", event);
                break;
            default:
                ESP_LOGW(TAG, "Unknown event: %d", event);
                break;
        
            }           
        } 
        if (bulb_state.is_on == false) {
            continue;
        }

            // No new event, continue with brightness control
        // ESP_LOGI(TAG, "No new event and timeout, continuing brightness control");
        // update auto_step_index
        if (auto_step_schedule_ms < esp_timer_get_time() / 1000)
        {
            auto_step_index = (auto_step_index + 1) % total_steps; // Loop through steps
            auto_step_schedule_ms = step_duration_ms + esp_timer_get_time() / 1000;
        }
            
        if (current_step_index == auto_step_index)
        {
            continue;
        }

        // update the RGB data based on current state
        if (is_rainbow_mode)
        {
            set_rainbow_color(auto_step_index * auto_step_size); // Set rainbow color for birthday
            current_step_index = auto_step_index;
        }

        send_rgb_to_strip();
    }

    // turn off the lightbulb
    vQueueDelete(bulb_state.lightbulb_task_queue);
    bulb_state.lightbulb_task_queue = NULL;
    bulb_state.lightbulb_task_handle = NULL;

}

/**
 * @brief initialize the lightbulb lowlevel module
 */
void lightbulb_init(void)
{

    ESP_LOGI(TAG, "Dummy Light Driver Init.");
    // Set default color
    bulb_state.red = DEFAULT_RED;
    bulb_state.green = DEFAULT_GREEN;
    bulb_state.blue = DEFAULT_BLUE;
    bulb_state.hue = 26.0f;
    bulb_state.saturation = 0.77f;
    bulb_state.value = 0.5f; // Full brightness
    bulb_state.is_on = false;

    // Initialize RMT for primary GPIO only
    esp_err_t ret = rmt_tx_init(RMT_TX_GPIO_PRIMARY, &tx_channel, &led_encoder);
    if (ret != ESP_OK)
    {
        tx_channel = NULL;
        return;
    }

    // Configure GPIO12 as an output pin
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;        // Disable interrupt on this pin
    io_conf.mode = GPIO_MODE_OUTPUT;              // Set as output mode
    io_conf.pin_bit_mask = (1ULL << 12);          // Bit mask for the pin
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; // Disable pull-down resistor
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;     // Disable pull-up resistor
    // gpio_config(&io_conf);                        // Apply the configuration

    // Set GPIO12 level to low
    // gpio_set_level(12, 0);

    xTaskCreate(lightbulb_task,
                "lightbulb_task",
                2048,
                NULL,
                2,
                &bulb_state.lightbulb_task_handle);
// Log initial color
}

/**
 * @brief turn on/off the lowlevel lightbulb
 */
int lightbulb_set_on(bool value)
{
    int event = LIGHTBULB_EVENT_ON;
    ESP_LOGI(TAG, "ON  : %s", value == true ? "true" : "false");


    // gpio_set_level(12, value);

    if (value == bulb_state.is_on)
    {
        return 0; // No state change needed
    }

    special_day_init_sntp(); // Initialize SNTP for special day handling
    bulb_state.is_on = value;
    if (value == false)
    {
        event = LIGHTBULB_EVENT_OFF; // Set event to OFF if turning off
    }

    xQueueSend(bulb_state.lightbulb_task_queue, &event, portMAX_DELAY);

    return 0;
}

/**
 * @brief set the saturation of the lowlevel lightbulb
 */
int lightbulb_set_saturation(float value)
{
    uint32_t event = LIGHTBULB_EVENT_REFRESH;
    ESP_LOGI(TAG, "lightbulb_set_saturation : %f", value);

    // Validate saturation (0.0 to 1.0)
    if (value < 0.0f || value > 1.0f)
    {
        ESP_LOGE(TAG, "Invalid saturation value: %f, must be between 0.0 and 1.0", value);
        return -1;
    }

    bulb_state.saturation = value;
    // update r,g,b
    hsv_to_rgb(bulb_state.hue, bulb_state.saturation, bulb_state.value, &bulb_state.red, &bulb_state.green, &bulb_state.blue);
    // Apply current brightness if bulb is on
    if (bulb_state.is_on && bulb_state.lightbulb_task_handle != NULL)
    {
        xQueueSend(bulb_state.lightbulb_task_queue, &event, portMAX_DELAY);
    }

    return 0;
}

/**
 * @brief set the hue of the lowlevel lightbulb
 */
int lightbulb_set_hue(float value)
{
    uint32_t event = LIGHTBULB_EVENT_HUE;
    ESP_LOGI(TAG, "lightbulb_set_hue : %f", value);

    // Validate hue (0.0 to 360.0)
    if (value < 0.0f || value > 360.0f)
    {
        ESP_LOGE(TAG, "Invalid hue value: %f, must be between 0.0 and 360.0", value);
        return -1;
    }

    bulb_state.hue = value;
    hsv_to_rgb(bulb_state.hue, bulb_state.saturation, bulb_state.value, &bulb_state.red, &bulb_state.green, &bulb_state.blue);

    // Apply current brightness if bulb is on
    if (bulb_state.is_on && bulb_state.lightbulb_task_handle != NULL)
    {
        xQueueSend(bulb_state.lightbulb_task_queue, &event, portMAX_DELAY);
    }

    return 0;
}

/**
 * @brief set the brightness of the lowlevel lightbulb
 */
int lightbulb_set_brightness(int value)
{
    uint32_t event = LIGHTBULB_EVENT_REFRESH;
    ESP_LOGI(TAG, "lightbulb_set_brightness : %d", value);

    bulb_state.value = value / 100.0f; // Convert to 0.0 to 1.0 range
    // send event to brightness control task
    if (bulb_state.lightbulb_task_handle != NULL)
    {
        xQueueSend(bulb_state.lightbulb_task_queue, &event, portMAX_DELAY);
    }
    return 0;
}

