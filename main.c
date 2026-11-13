/*
TODO:
    1. Average the values before displaying
    2. Check what is causing the constant fluctuation in readings
*/
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/dac_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "oled.h"

#define MASTER_SDA GPIO_NUM_21              // GPIO for SDA of ADS1115
#define MASTER_SCL GPIO_NUM_22              // GPIO for SCL of ADS1115
#define ALRT GPIO_NUM_4                     // GPIO for ALRT of ADS1115
#define ADS1115_ADDR 0x48                   // Address of ADS1115

i2c_master_bus_handle_t adc_handle;         // Master Bus Handle
i2c_master_dev_handle_t ads1115_handle;     // ADS1115 Handle

TaskHandle_t ADCTask_handle;                // Handle for the ADC task
TaskHandle_t DropHandle;                    // Handle for the Drop counter
TaskHandle_t MiddleHandle;                  // Handle for the middle warning
TaskHandle_t BottomHandle;                  // Handle for the bottom warning
TaskHandle_t screenHandle;                  // Handle for the screen updater

SemaphoreHandle_t adcISR_handle;            // Handle for the interrupt service

QueueHandle_t DropQueue;                    // Queue to get the drop values
QueueHandle_t MiddleQueue;                  // Queue to get the middle values
QueueHandle_t BottomQueue;                  // Queue to get the bottom values

dac_oneshot_handle_t DACtop_handle;         // Handle for the DAC controlling top 2 transmitters
dac_oneshot_handle_t DACbottom_handle;      // Handle for the DAC controlling bottom transmitter

uint16_t top_ref, mid_ref, bot_ref;         // Reference for the transmitters
volatile uint32_t dripRate;                 // Drip rate
volatile uint32_t dropCount = 0;            // Total drop detected
volatile uint8_t middleWarn = 0;            // Warn if the middle level changes
volatile uint8_t bottomWarn = 0;            // Warn if the bottom level changes

// Read the ADC value via I2C
void readADC(uint8_t *read_buffer)
{
    const uint8_t write_buffer = 0x00;
    // Read the current value in the converion register
    ESP_ERROR_CHECK(i2c_master_transmit_receive(ads1115_handle, &write_buffer, 1, read_buffer, 2, -1));
}

// Read the current configuration of the ads1115
void readADC_config(uint8_t *read_buffer)
{
    const uint8_t write_buffer = 0x01;
    // Read the current configuration register
    ESP_ERROR_CHECK(i2c_master_transmit_receive(ads1115_handle, &write_buffer, 1, read_buffer, 2, -1));
}

// Set the config for ads1115
void writeADC_config(uint8_t *write_buffer)
{
    uint8_t write_arg [3] = {0x01, write_buffer[0], write_buffer[1]};
    // Write the new config to the configuration register
    ESP_ERROR_CHECK(i2c_master_transmit(ads1115_handle, write_arg, 3, -1));
}

// Write to any 16-bit ADS1115 register
void writeADC_Register(uint8_t reg_pointer, uint16_t value)
{
    uint8_t write_payload[3];
    write_payload[0] = reg_pointer;
    write_payload[1] = (value >> 8) & 0xFF; // MSB
    write_payload[2] = value & 0xFF;        // LSB
    
    ESP_ERROR_CHECK(i2c_master_transmit(ads1115_handle, write_payload, 3, -1));
}

// Set the ADC channel
void setADC_Channel(uint8_t channel)
{
    uint8_t write_buffer[2] = {0xC5, 0xE0};
    switch (channel) 
    {
        case 0:
        {
            write_buffer[0] = 0xC5;         // Setting the Channel to AIN0 and initializing the single shot reading
            break;
        }
        case 1:
        {
            write_buffer[0] = 0xD5;         // Setting the Channel to AIN1 and initializing the single shot reading
            break;
        }
        case 2:
        {
            write_buffer[0] = 0xE5;         // Setting the Channel to AIN2 and initializing the single shot reading
            break;
        }
        case 3:
        {
            write_buffer[0] = 0xF5;         // Setting the Channel to AIN3 and initializing the single shot reading
            break;
        }
        default:
        {
            write_buffer[0] = 0xC5;         // Setting the Channel to AIN0 and initializing the single shot reading
            break;
        }
    }
    // Writing the config
    writeADC_config(write_buffer);
}

// Hardware Interrupt Handler (Must be in IRAM for fast execution)
static void IRAM_ATTR adc_rdy_isr_handler(void* arg) 
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Unblock the ADCTask instantly
    xSemaphoreGiveFromISR(adcISR_handle, &xHigherPriorityTaskWoken);
    
    // Yield the processor if ADCTask is higher priority than the currently running task
    if (xHigherPriorityTaskWoken) 
    {
        portYIELD_FROM_ISR();
    }
}

// Initializes the I2C for ADC
void init_ADC(void)
{
    static const char *TAG_ADC_CONFIG = "ADC_CONFIG";

    adcISR_handle = xSemaphoreCreateBinary();

    gpio_config_t alrt_ads1115 = {
        .pin_bit_mask = 1ULL << ALRT,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE          // Triggers if the ALRT pin goes low
    };
    gpio_config(&alrt_ads1115);

    // Install ISR service
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ALRT, adc_rdy_isr_handler, NULL);

    // I2C master config
    i2c_master_bus_config_t adc = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = MASTER_SDA,
        .scl_io_num = MASTER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 2
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&adc, &adc_handle));

    // I2C device config
    i2c_device_config_t ads1115 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = 200000,
        .scl_wait_us = 0
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(adc_handle, &ads1115, &ads1115_handle));

    uint8_t initialConfig[2] = {0x45, 0xE0};    // Sets the SPS to 860 and channel to AIN0
    uint8_t expectedRead[2] = {0xC5, 0xE0};
    uint8_t readInitialConfig [2] = {0x00};

    // Sets the Lo and Hi register for conversion indication on ALRT pin
    writeADC_Register(0x02, 0x0000); // Lo_thresh MSB = 0
    writeADC_Register(0x03, 0x8000); // Hi_thresh MSB = 1

    // Write the Initial Config
    writeADC_config(initialConfig);
    vTaskDelay(pdMS_TO_TICKS(10));
    readADC_config(readInitialConfig);

    // Checks if the config has been set
    if (memcmp((void *)expectedRead, (void *)readInitialConfig, 2) == 0)
    {
        ESP_LOGI(TAG_ADC_CONFIG, "Initial configuration is complete");
    }
    else 
    {
        ESP_LOGE(TAG_ADC_CONFIG, "Unable to set the configuration");
    }
}

// ADC measurement Task
void ADCTask(void *pvParameters)
{
    TickType_t xLastTime = xTaskGetTickCount();     // Gets the current tick count

    uint8_t readBuffer[2];
    uint8_t currChannel = 0;
    uint16_t adc_value = 0;

    while (1)
    {
        xSemaphoreTake(adcISR_handle, 0);
        // Samples the channel in currChannel
        setADC_Channel(currChannel);
        // Waits fro 1.2ms
        //esp_rom_delay_us(1200);
        // Wait till the conversion is ready
        xSemaphoreTake(adcISR_handle, portMAX_DELAY);
        // Read the adc value
        readADC(readBuffer);

        // Converts the read bytes to integer
        adc_value = (readBuffer[0] << 8) | (readBuffer[1]);
        
        // Normalizing the adc values
        adc_value = (adc_value & 0x8000) ? 0 : adc_value;

        // Send the adc value to appropritate queue
        if (currChannel == 0)
        {
            xQueueSend(DropQueue, &adc_value, 0);
        }
        else if (currChannel == 1) 
        {
            xQueueSend(MiddleQueue, &adc_value, 0);
        }
        else
        {
            xQueueSend(BottomQueue, &adc_value, 0);
        }

        // Increments the channel
        currChannel += 1;
        if(currChannel > 2)
        {
            currChannel = 0;
        }

        // Wait 5ms max
        vTaskDelayUntil(&xLastTime, pdMS_TO_TICKS(5));
    }
}

// Drip Counter
void dripCounter(void *pvParameters)
{
    uint16_t current;
    uint8_t drop = 0;
    uint32_t empty = 0;
    TickType_t DropPrev = 0;
    TickType_t DropCurr;

    while (1) 
    {
        if (xQueueReceive(DropQueue, &current, portMAX_DELAY))
        {
            ESP_LOGI("DROP", "%u (%u)", current, top_ref);
            // Starts the time when a drop is identified
            if (current < (top_ref - 1500))
            {
                if (drop == 0)
                {
                    DropCurr = xTaskGetTickCount();
                }
                drop += 1;
                empty = 0;
            }
            // Calculates the driprate when the drop is no longer detected
            else 
            {
                if (empty > 750)
                {
                    dripRate = 0;
                }
                if (drop >= 4)
                {
                    if(DropPrev != 0)
                    {
                        dripRate += DropCurr - DropPrev;
                    }
                    DropPrev = DropCurr;
                    dropCount += 1;
                }
                empty += 1;
                drop = 0;
            }
        }
    }
}

// Middle Diode
void middleLevel(void *pvParameters)
{
    uint16_t current;
    uint8_t levelChange = 0;

    while (1) 
    {
        if (xQueueReceive(MiddleQueue, &current, portMAX_DELAY))
        {
            // ESP_LOGI("MIDDLE", "%u (%u)", current, mid_ref);
            // Counts the number of abrnormalities 
            if (current < (mid_ref - 1500))
            {
                if (levelChange > 200){
                    middleWarn = 1;
                }
                levelChange += 1;
            }
            // Resets the counter when the operation returns to normal 
            else 
            {
                levelChange = 0;
                middleWarn = 0;
            }
        }
    }
}

// Bottom Diode
void bottomLevel(void *pvParameters)
{
    uint16_t current;
    uint8_t levelChange = 0;

    while (1) 
    {
        if (xQueueReceive(BottomQueue, &current, portMAX_DELAY))
        { 
            // ESP_LOGI("BOTTOM", "%u (%u)", current, bot_ref);
            // Counts the number of abrnormalities 
            if (current < (bot_ref - 500))
            {
                if (levelChange > 200){
                    bottomWarn = 1;
                }
                levelChange += 1;
            }
            // Resets the counter when the operation returns to normal 
            else 
            {
                levelChange = 0;
                bottomWarn = 0;
            }
        }
    }
}

// Initialize the Transmitter
void transmitterInit(uint16_t *ambient)
{
    static const char *TAG_DAC = "TRANS_INIT";

    // Initializing the DAC for the top transmitters
    dac_oneshot_config_t DACconfig_Top = {
        .chan_id = DAC_CHAN_0
    };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&DACconfig_Top, &DACtop_handle));

    // Initializing the DAC for the bottom transmitter
    dac_oneshot_config_t DACconfig_Bottom = {
        .chan_id = DAC_CHAN_1
    };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&DACconfig_Bottom, &DACbottom_handle));

    // Checking for high ambient light
    if (ambient[0] > 28000 || ambient[1] > 28000) 
    {
        ESP_LOGE(TAG_DAC, "Too much ambient light");
    }
    else
    {
        uint8_t value = 150;
        // A negative feedback loop to set the appropriate temperature for the top transmitters
        while (1)
        {
            ESP_ERROR_CHECK(dac_oneshot_output_voltage(DACtop_handle, value));
            vTaskDelay(pdMS_TO_TICKS(2));

            uint8_t readVal[2];
            uint16_t adc_value;

            xSemaphoreTake(adcISR_handle, 0);
            setADC_Channel(0);
            xSemaphoreTake(adcISR_handle, portMAX_DELAY);
            readADC(readVal);
            // Converts the read bytes to integer
            adc_value = (readVal[0] << 8) | (readVal[1]);
            // Normalizing the adc values
            adc_value = (adc_value & 0x8000) ? 0 : adc_value;

            if ((adc_value - ambient[0]) > 4000)
            {
                ESP_LOGI(TAG_DAC, "Appropriate brightness set for top");
                break;
            }

            value += 10;
            // Exiting in case of max brightness
            if (value > 250)
            {
                ESP_LOGW(TAG_DAC, "Max brigthness reached at top");
                break;
            }
        }

        value = 150;
        // A negative feedback loop to set brightness for bottom transmitter
        while (1) 
        {
            ESP_ERROR_CHECK(dac_oneshot_output_voltage(DACbottom_handle, value));
            vTaskDelay(pdMS_TO_TICKS(2));

            uint8_t readVal[2];
            uint16_t adc_value;

            xSemaphoreTake(adcISR_handle, 0);
            setADC_Channel(2);
            xSemaphoreTake(adcISR_handle, portMAX_DELAY);
            readADC(readVal);
            // Converts the read bytes to integer
            adc_value = (readVal[0] << 8) | (readVal[1]);
            // Normalizing the adc values
            adc_value = (adc_value & 0x8000) ? 0 : adc_value;

            if ((adc_value - ambient[1]) > 4000)
            {
                ESP_LOGI(TAG_DAC, "Appropriate brightness set for bottom");
                break;
            }

            value += 10;
            // Exiting in case of max brightness
            if (value > 250)
            {
                ESP_LOGW(TAG_DAC, "Max brigthness reached at bottom");
                break;
            }
        }
        uint8_t readVal[2];
        uint16_t adc_value;

        for (int i = 0; i < 3; i++)
        {
            xSemaphoreTake(adcISR_handle, 0);
            setADC_Channel(i);
            xSemaphoreTake(adcISR_handle, portMAX_DELAY);
            readADC(readVal);
            // Converts the read bytes to integer
            adc_value = (readVal[0] << 8) | (readVal[1]);
            // Normalizing the adc values
            adc_value = (adc_value & 0x8000) ? 0 : adc_value;
            if (i == 0)
            {
                top_ref = adc_value;
            }
            else if (i == 1) 
            {
                mid_ref = adc_value;
            }
            else 
            {
                bot_ref = adc_value;
            }
        }
    }
}

// Initial Calibration
void calibrateADC(void)
{
    static const char *TAG_CAL = "CALIBRATION";

    uint8_t ambient[2];
    uint16_t adcVal[5][2];

    uint8_t i = 0;

    // Take 5 samples from the top and bottom 
    while (i < 5)
    {
        setADC_Channel(0);
        xSemaphoreTake(adcISR_handle, portMAX_DELAY);
        readADC(ambient);

        adcVal[i][0] = (ambient[0] << 8) | (ambient[1]);
        adcVal[i][0] = (adcVal[i][0] & 0x8000) ? 0 : adcVal[i][0];

        setADC_Channel(2);
        xSemaphoreTake(adcISR_handle, portMAX_DELAY);
        readADC(ambient);

        adcVal[i][1] = (ambient[0] << 8) | (ambient[1]);
        adcVal[i][1] = (adcVal[i][1] & 0x8000) ? 0 : adcVal[i][1];

        vTaskDelay(pdMS_TO_TICKS(50));
        i += 1;
    }

    // Take the average of the top and the bottom for normalizing
    uint16_t norm_ambient[2] = {
        (adcVal[0][0]+adcVal[1][0]+adcVal[2][0]+adcVal[3][0]+adcVal[4][0])/5, 
        (adcVal[0][1]+adcVal[1][1]+adcVal[2][1]+adcVal[3][1]+adcVal[4][1])/5
    };

    ESP_LOGI(TAG_CAL, "Ambient light: Top %u Bottom %u", norm_ambient[0], norm_ambient[1]);

    transmitterInit(norm_ambient);
}

// Screen Updater
void updateScr(void *pvParameters)
{
    static const char *TAG_SCR = "LOG";
    uint32_t prevCount = 0;
    uint32_t averageDrop = 0;

    while(1)
    {
        ESP_LOGI(TAG_SCR, "Drip rate: %lu", dripRate);
        ESP_LOGI(TAG_SCR, "Middle level: %d", middleWarn);
        ESP_LOGI(TAG_SCR, "Bottom level: %d", bottomWarn);

        if (dropCount > prevCount)
        {
            averageDrop = ((uint32_t)dripRate) / (dropCount - prevCount);
            prevCount = dropCount;
            dripRate = 0; // Reset dripRate tally after averaging
        }
        else 
        {
            // If no new drops fell in the last 2 seconds, the rate is effectively 0
            averageDrop = 0;
        }

        // Update the screen
        update_live_data(pdTICKS_TO_MS(averageDrop), dropCount, middleWarn, bottomWarn);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    // Creating the queues
    DropQueue = xQueueCreate(10, sizeof(uint16_t));
    MiddleQueue = xQueueCreate(10, sizeof(uint16_t));
    BottomQueue = xQueueCreate(10, sizeof(uint16_t));

    // Initialize the I2C and configure ADS1115
    init_ADC();
    calibrateADC();

    vTaskDelay(pdMS_TO_TICKS(50));

    // Initialize the screen and get the menu inputs
    init_oled_menu();

    // Have the ADC Task pinned to Core 1
    xTaskCreatePinnedToCore(ADCTask, "ADC Task", 2*1024, NULL, 8, &ADCTask_handle, 1);

    // Task to count the drop rate
    xTaskCreatePinnedToCore(dripCounter, "Drip Counter", 2*1024, NULL, 5, &DropHandle, 0);
    xTaskCreatePinnedToCore(middleLevel, "Middle Warning", 2*1024, NULL, 5, &MiddleHandle, 0);
    xTaskCreatePinnedToCore(bottomLevel, "Bottom Warning", 2*1024, NULL, 5, &BottomHandle, 0);

    // 100 ms delay for gearing up
    vTaskDelay(pdMS_TO_TICKS(100));

    // Start the screen interface
    xTaskCreatePinnedToCore(updateScr, "Update Screen", 2*1024, NULL, 3, &screenHandle, 0);
}
