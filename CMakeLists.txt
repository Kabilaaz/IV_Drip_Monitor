idf_component_register(SRCS "main.c" "oled.c"
                    INCLUDE_DIRS "."
	    	    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_dac esp_driver_spi)
