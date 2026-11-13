#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "ssd1306.h" 

// --- BUTTON & I2C PINS ---
#define BTN_SCROLL GPIO_NUM_32
#define BTN_SELECT GPIO_NUM_33
#define I2C_SDA_PIN 18
#define I2C_SCL_PIN 19

// --- MENU STATES & DATA ---
typedef enum { STATE_SELECT_VOLUME, STATE_SELECT_TUBE, STATE_RUNNING } MenuState;
const int volumes[] = {100, 250, 500, 1000};
const int drop_factors[] = {10, 15, 20, 60};

MenuState current_state = STATE_SELECT_VOLUME;
int vol_index = 0;
int tube_index = 0;
bool needs_display_update = true;

// --- LIVE SENSOR DATA ---
static uint32_t live_drip_rate_ms = 0;
static uint32_t live_drops_passed = 0; 
static uint8_t live_mid_warn = 0;
static uint8_t live_bot_warn = 0;

SSD1306_t dev; 

void init_buttons() {
    gpio_config_t btn_config = {};
    btn_config.pin_bit_mask = (1ULL << BTN_SCROLL) | (1ULL << BTN_SELECT);
    btn_config.mode = GPIO_MODE_INPUT;
    btn_config.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&btn_config);
}

void update_oled_display() {
    if (!needs_display_update) return;

    ssd1306_clear_screen(&dev, false); 
    char buf[32]; 

    if (current_state == STATE_SELECT_VOLUME) {
        ssd1306_display_text(&dev, 0, ">> STEP 1 <<", 12, false);
        ssd1306_display_text(&dev, 2, "Select Vol:", 11, false);
        
        sprintf(buf, "[ %d mL ]", volumes[vol_index]);
        ssd1306_display_text(&dev, 4, buf, strlen(buf), true); 
    } 
    else if (current_state == STATE_SELECT_TUBE) {
        ssd1306_display_text(&dev, 0, ">> STEP 2 <<", 12, false);
        ssd1306_display_text(&dev, 2, "IV Tube Type:", 13, false);
        
        sprintf(buf, "[ %d drops/mL ]", drop_factors[tube_index]);
        ssd1306_display_text(&dev, 4, buf, strlen(buf), true);
    } 
    else if (current_state == STATE_RUNNING) {
        // --- ALERT OVERRIDE ---
        if (live_mid_warn == 1 || live_bot_warn == 1) {
            ssd1306_display_text(&dev, 0, "!!! ALERT !!!", 13, true);
            
            if (live_mid_warn == 1) {
                ssd1306_display_text(&dev, 3, "OVERFLOW", 8, false);
                ssd1306_display_text(&dev, 4, "DETECTED", 8, false);
            } else if (live_bot_warn == 1) {
                ssd1306_display_text(&dev, 3, "EMPTY IV", 8, false);
                ssd1306_display_text(&dev, 4, "LINE", 4, false);
            }
            ssd1306_display_text(&dev, 7, "CHECK PATIENT", 13, true);
        } 
        // --- NORMAL MONITORING ---
        else {
            ssd1306_display_text(&dev, 0, "> MONITORING <", 14, true);
            
            uint32_t dpm = 0;
            uint32_t time_left_min = 0;
            
            // 1. Calculate how many drops are actually left in the bag
            uint32_t total_drops_in_bag = volumes[vol_index] * drop_factors[tube_index];
            uint32_t drops_left = (total_drops_in_bag > live_drops_passed) ? (total_drops_in_bag - live_drops_passed) : 0;
            
            // 2. Calculate remaining volume based on remaining drops
            uint32_t vol_left = drops_left / drop_factors[tube_index];

            // 3. Apply the dynamic math using the live speed to get remaining time
            if (live_drip_rate_ms > 0) {
                dpm = 60000 / live_drip_rate_ms;
                time_left_min = (drops_left * live_drip_rate_ms) / 60000;
            }

            sprintf(buf, "Rate: %lu DPM", dpm);
            ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
            
            sprintf(buf, "Time: %lu min", time_left_min);
            ssd1306_display_text(&dev, 4, buf, strlen(buf), false);
            
            sprintf(buf, "Left: %lu mL", vol_left);
            ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
        }
    }
    
    needs_display_update = false;
}

void menu_task(void *pvParameter) {
    int last_scroll = 1, last_select = 1;

    while (1) {
        int current_scroll = gpio_get_level(BTN_SCROLL);
        int current_select = gpio_get_level(BTN_SELECT);

        if (last_scroll == 1 && current_scroll == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); 
            if (current_state == STATE_SELECT_VOLUME) {
                vol_index = (vol_index + 1) % 4;
            } else if (current_state == STATE_SELECT_TUBE) {
                tube_index = (tube_index + 1) % 4;
            }
            needs_display_update = true;
        }

        if (last_select == 1 && current_select == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); 
            if (current_state == STATE_SELECT_VOLUME) {
                current_state = STATE_SELECT_TUBE;
            } else if (current_state == STATE_SELECT_TUBE) {
                current_state = STATE_RUNNING;
            }
            needs_display_update = true;
        }

        last_scroll = current_scroll;
        last_select = current_select;

        update_oled_display();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void init_oled_menu(void) {
    i2c_master_init(&dev, I2C_SDA_PIN, I2C_SCL_PIN, -1);
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);

    init_buttons();
    xTaskCreate(menu_task, "menu_task", 4096, NULL, 5, NULL);
}

// --- DATA PIPELINE FUNCTIONS FOR MAIN.C ---

void update_live_data(uint32_t drip_rate_ms, uint32_t drops_passed, uint8_t mid_warn, uint8_t bot_warn) {
    if (live_drip_rate_ms != drip_rate_ms || live_drops_passed != drops_passed || live_mid_warn != mid_warn || live_bot_warn != bot_warn) {
        live_drip_rate_ms = drip_rate_ms;
        live_drops_passed = drops_passed;
        live_mid_warn = mid_warn;
        live_bot_warn = bot_warn;
        needs_display_update = true;
    }
}

bool is_menu_finished(void) {
    return (current_state == STATE_RUNNING);
}

int get_selected_volume(void) {
    return volumes[vol_index];
}

int get_selected_drop_factor(void) {
    return drop_factors[tube_index];
}