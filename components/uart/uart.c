/*
 * MIT License
 *
 * Copyright (c) 2017 David Antliff
 * Copyright (c) 2017 Chris Morgan <chmorgan@gmail.com>
 *
*/
#include "esp_log.h"
#include "inttypes.h"
#include "uart.h"
#include "driver/uart.h"

#define TAG "uart"

// static const char * TAG = "uart";

	//setup UART
	const int uart_buffer_size = (1024 * 2);

void uart_init()
{	uart_config_t uart_config = {
		.baud_rate = 115200,
		//.baud_rate = 921600,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
	uart_param_config(UART_NUM_0, &uart_config);
	uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	//uart_driver_install(UART_NUM_0, uart_buffer_size, 0, 0, NULL, 0); //no TX buffer??
	uart_driver_install(UART_NUM_0, uart_buffer_size, uart_buffer_size, 0, NULL, 0); //with TX buffer??
}

void uart_write(char *data, uint8_t len)
{
    int size;
    //size = uart_tx_chars(UART_NUM_0, data, len);
	//using tx_chars sends only a few bytes, and seems to be blocked by esp_log!!
	//maybe uart_fill_fifo is interesting in ISR on DRDY L
    size = uart_write_bytes(UART_NUM_0, data, len);
    if (size > 0)
    {
        ESP_LOGI(TAG, "UART Wrote %d bytes", size);
    }
    else
    {
        ESP_LOGI(TAG, "UART Comm Failed");
    }
}
