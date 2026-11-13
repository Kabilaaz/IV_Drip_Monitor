#ifndef OLED_H
#define OLED_H

#include <stdbool.h>
#include <stdint.h>

// Initializes the I2C bus for the OLED, configures buttons, and spawns the menu task
void init_oled_menu(void);

// Getter functions for main.c to read the user's selections
bool is_menu_finished(void);
int get_selected_volume(void);
int get_selected_drop_factor(void);

// Setter function to push live sensor data to the screen (Now includes drops_passed)
void update_live_data(uint32_t drip_rate_ms, uint32_t drops_passed, uint8_t mid_warn, uint8_t bot_warn);

#endif // OLED_H