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
#include "driver/gpio.h"

#define TAG "uart"

//setup UART
const int uart_buffer_size = (1024 * 2); //less would possibly be OK

void uart_init()
{
	uart_config_t uart_config = {
		//.baud_rate = 115200,
		//.baud_rate = 921600,
		//.baud_rate = 2000000,
		.baud_rate = 3000000,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
	uart_param_config(UART_NUM_0, &uart_config);
	uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_driver_install(UART_NUM_0, uart_buffer_size, 0, 0, NULL, 0); //no TX buffer??																	  //uart_driver_install(UART_NUM_0, uart_buffer_size, uart_buffer_size, 0, NULL, 0); //with TX buffer??
																	  //uart_driver_install(UART_NUM_0, uart_buffer_size, uart_buffer_size, 0, NULL, 0); //no TX buffer??																	  //uart_driver_install(UART_NUM_0, uart_buffer_size, uart_buffer_size, 0, NULL, 0); //with TX buffer??
}

// Hmm uart_tx_chars works with and without tx buffer ... I guess ESP_LOG is dangerous
// as a combination ....

void uart_write(char *data, size_t len)
{
	int size;

	//uart_wait_tx_done(UART_NUM_0, portMAX_DELAY); //last transfer
	//int count = UART0.status.txfifo_cnt; //should tell us how much is in there
	uint8_t tx_remain_fifo_cnt = 0;
	/*do
	{
		uint8_t tx_fifo_cnt = UART0.status.txfifo_cnt;
		tx_remain_fifo_cnt = (UART_FIFO_LEN - tx_fifo_cnt);
		gpio_set_level(GPIO_NUM_33, 1);
		gpio_set_level(GPIO_NUM_33, 0); //to scope
	} while (tx_remain_fifo_cnt < len);*/

    
    //UART0.conf0.txfifo_rst = 1; //clear TX_FIFO...

	uint8_t tx_fifo_cnt = UART0.status.txfifo_cnt;
	tx_remain_fifo_cnt = (UART_FIFO_LEN - tx_fifo_cnt);
	if (tx_remain_fifo_cnt >= len) //only transmit if space permits 
	{
		size = uart_tx_chars(UART_NUM_0, data, len);
	}
	else
	{
		gpio_set_level(GPIO_NUM_33, 1);
		//ets_delay_us(len-tx_remain_fifo_cnt);
		gpio_set_level(GPIO_NUM_33, 0); //to scope
	}

	/* Funny it seems we are transmitting too early for some data rates...why?
Using reg 1 values from 96h to 92h (250 SPS to 4000SPS) we get no problem and can easily write 
(and receive) even in hex textmode ...
if we go to to 91h or 90h (8K 16K) the buffer has not enough space for the new stuff
--> we should tx a bit later but not too late .... how can this be done ??
if we put a wait_tx_done BEFORE ?  --> everything screws up .... maybe because we do not need an EMPTY
buffer... 
*/


//In UART0, bit UART_TXFIFO_RST and bit UART_RXFIFO_RST can be set to reset Tx_FIFO or Rx_FIFO,respectively. 

	//size = uart_write_bytes(UART_NUM_0, data, len);

	/*if (size < len)
	{
		gpio_set_level(GPIO_NUM_33, 1);
		ets_delay_us(1); // signal collison on scope
		gpio_set_level(GPIO_NUM_33, 0);
	}*/
	//uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
	// this is the safe option, but means we will only be fast enough to SPS 4000

	//size = uart_write_bytes(UART_NUM_0, data, len);
	//int len2 = sprintf(buf, "written %d received %d\n", size,len);
	//size = uart_tx_chars(UART_NUM_0, buf, len2);
	//size = uart_write_bytes(UART_NUM_0, buf, len2);

	//using tx_chars sends only a few bytes, and seems to be blocked by esp_log!!
	//maybe uart_fill_fifo is interesting in ISR on DRDY L
	//size = uart_write_bytes(UART_NUM_0, data, len);
	/*if (size > 0)
    {
        ESP_LOGI(TAG, "UART Wrote %d bytes", size);
    }
    else
    {
        ESP_LOGI(TAG, "UART Comm Failed");
    }*/
}
