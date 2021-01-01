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

#define DRDY_PIN GPIO_NUM_27 //INPUT with ISR falling edge

#define CS_PIN GPIO_NUM_5 //OUTPUT def H pull L to start
#define RESET_PIN GPIO_NUM_25 //OUTPUT def H pull L to reset

#define CLKSEL_PIN GPIO_NUM_33 //OUTPUT def L using ext clock
#define START_PIN GPIO_NUM_26 //OUTPUT def L to use commands
#define LED_PIN GPIO_NUM_32 //OUTPUT def L = LED off

#include <stdint.h>


extern volatile uint8_t spi_data_available;

void spi_init();
uint8_t spiRec();
uint8_t spiRec(uint8_t *buf, uint8_t len);
void spiSend(uint8_t b);
void spiSend(uint8_t *buf, uint8_t len);

void adcSendCommand(int cmd);
void adcSendCommandLeaveCsActive(int cmd);
void adcWreg(int reg, int val);
int adcRreg(int reg);

#endif // _ADS_COMMAND_H
