/*
 * adsCommand.h
 *
 * Copyright (c) 2013-2019 by Adam Feuer <adam@adamfeuer.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _ADS_COMMAND_H
#define _ADS_COMMAND_H

#define ESP_INTR_FLAG_DEFAULT 0

#define DRDY_PIN GPIO_NUM_17 //INPUT with ISR falling edge

#define CS_PIN GPIO_NUM_5 //OUTPUT def H pull L to start
#define RESET_PIN GPIO_NUM_32 //OUTPUT def H pull L to reset

#define CLKSEL_PIN GPIO_NUM_16 //OUTPUT def L using ext clock
#define START_PIN GPIO_NUM_25 //OUTPUT def L to use commands
#define LED_PIN GPIO_NUM_33 //OUTPUT def L = LED off

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t xSemaphore;

//extern volatile uint8_t spi_data_available;
extern volatile bool is_rdatac;
extern volatile bool is_rdata;
extern volatile uint32_t current_sample;
extern volatile bool handling_data;
extern TaskHandle_t rdatac_task_handle;
extern TaskHandle_t read_task_handle;


void spi_init();
uint8_t spiRec();
uint8_t spiRec(uint8_t *buf, uint8_t len);
void spiSend(uint8_t b);
void spiSend(uint8_t *buf, uint8_t len);

void adcSendCommand(uint8_t cmd);
//void adcSendCommandLeaveCsActive(int cmd);
void adcWreg(uint8_t reg, uint8_t val);
uint8_t adcRreg(uint8_t reg);

#endif // _ADS_COMMAND_H
