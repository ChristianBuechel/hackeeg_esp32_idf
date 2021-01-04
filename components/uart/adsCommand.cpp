/**
 * send and receive commands from TI ADS129x chips.
 *
 * Copyright (c) 2013 by Adam Feuer <adam@adamfeuer.com>
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
#include "string.h"
#include "adsCommand.h"
#include "ads129x.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/task.h"

spi_bus_config_t buscfg;
spi_device_interface_config_t devcfg;
spi_device_handle_t spi;

volatile uint8_t spi_data_available;


static void IRAM_ATTR drdy_interrupt(void *arg)
{
    spi_data_available = 1;
}

void spi_init() //probably need to re-init when transfering data at hign speed
{
    buscfg.mosi_io_num = GPIO_NUM_23;
    buscfg.miso_io_num = GPIO_NUM_19;
    buscfg.sclk_io_num = GPIO_NUM_18;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 64; //should be enough


    devcfg.clock_speed_hz = 4 * 1000 * 1000; //Using 4 MHz mean we can send multibyte stuff in one go
                                             //hopefully we can chage that for data transfer
                                             //works (RDATAC) with 14 MHz
    devcfg.mode = 1;                         //SPI mode 1 p.12 CPOL = 0 and CPHA = 1.
    //devcfg.cs_ena_posttrans = 4;           //p.38 ADS1299 data sheet NOT needed if CS driven manaully
    //devcfg.spics_io_num = CS_PIN;          //let esp operate CS pin
    //BUT then keep CS_PIN out of GPIO stuff otherwise it does not work
    devcfg.spics_io_num = -1;                //we operate CS pin manually use -1
    devcfg.queue_size = 1;                   //only one transactions at a time
    //devcfg.flags = SPI_DEVICE_HALFDUPLEX;    // try half duplex
    spi_bus_initialize(VSPI_HOST, &buscfg, 0); //no DMA
    spi_bus_add_device(VSPI_HOST, &devcfg, &spi);

    gpio_config_t gp;
    gp.intr_type = GPIO_INTR_DISABLE;
    gp.mode = GPIO_MODE_OUTPUT;
    gp.pull_up_en = GPIO_PULLUP_ENABLE;
    gp.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gp.pin_bit_mask = (1ULL << CS_PIN) | (1ULL << RESET_PIN);
    //gp.pin_bit_mask = (1ULL << RESET_PIN); if CS_PIN is esp controlled
    gpio_config(&gp);
    gp.pull_up_en = GPIO_PULLUP_DISABLE;
    gp.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gp.pin_bit_mask = (1ULL << CLKSEL_PIN) | (1ULL << START_PIN) | (1ULL << LED_PIN);
    gpio_config(&gp);


    gp.mode = GPIO_MODE_INPUT;
    gp.intr_type = GPIO_INTR_NEGEDGE;
    gp.pull_up_en = GPIO_PULLUP_ENABLE;
    gp.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gp.pin_bit_mask = 1ULL << DRDY_PIN;
    gpio_config(&gp);
   
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(DRDY_PIN, drdy_interrupt, (void *)DRDY_PIN);
    

    //startup p.62
    gpio_set_level(LED_PIN, 0);    // LED off
    gpio_set_level(CLKSEL_PIN, 0); // use external clock
    //gpio_set_level(CLKSEL_PIN, 1); // use internal clock like hackeeg
    gpio_set_level(RESET_PIN, 1);  // RESET H
    //vTaskDelay(100);               //now wait 2^18 tCLK = 128ms (13) but start with 1000ms (100)
    vTaskDelay(1000 / portTICK_PERIOD_MS);//now wait 2^18 tCLK = 128ms (13) but start with 1000ms (100)
    gpio_set_level(RESET_PIN, 0);  // RESET !
    ets_delay_us(10);              //2 tCLK = 0.9 us (1) but start with 10 us
    gpio_set_level(RESET_PIN, 1);  // done

    gpio_set_level(START_PIN, 0); // control by command

    gpio_set_level(CS_PIN, 1);    // CS H
    //gpio_set_level(RESET_PIN, 1); // RESET H
}

/** SPI receive a byte */
uint8_t spiRec()
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.flags = SPI_TRANS_USE_RXDATA;
    spi_device_polling_transmit(spi, &t);
    return *(uint8_t *)t.rx_data;
}

//uint8_t tx_data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t tx_data[100] = {0};
//memset(tx_data, 0, sizeof(tx_data));
/** SPI receive multiple bytes */
uint8_t spiRec(uint8_t *buf, uint8_t len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * len;
    t.rx_buffer = buf;
    //t.tx_buffer = NULL; //no sending data !!
    t.tx_buffer = tx_data; // sending zeros !!
    t.flags     = 0; 
    spi_device_polling_transmit(spi, &t);
    return 0;
}

/** SPI send a byte */
void spiSend(uint8_t b)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));             //Zero out the transaction
    t.length = 8;                         //Command is 8 bits
    t.tx_buffer = &b;                     //The data is the cmd itself
    spi_device_polling_transmit(spi, &t); //Transmit!
}

/** SPI send multiple bytes */
void spiSend(uint8_t *buf, uint8_t len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));             //Zero out the transaction
    t.length = 8 * len;                   //Command is 8 bits
    t.tx_buffer = buf;                    //The data is the cmd itself
    spi_device_polling_transmit(spi, &t); //Transmit!
}

void adcSendCommand(int cmd)
{
    /*spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=8;                     //Command is 8 bits
    t.tx_buffer=&cmd;               //The data is the cmd itself
    spi_device_polling_transmit(spi, &t);  //Transmit! */
    gpio_set_level(CS_PIN, 0); 
    spiSend(cmd); //SCLK appears about 10us after CS L
    ets_delay_us(1); //wait 1us
    gpio_set_level(CS_PIN, 1);
    /*digitalWrite(PIN_CS, LOW);
    spiSend(cmd);
    delayMicroseconds(1);
    digitalWrite(PIN_CS, HIGH);*/
}

void adcSendCommandLeaveCsActive(int cmd)
{
    /*digitalWrite(PIN_CS, LOW);
    spiSend(cmd);*/
    gpio_set_level(CS_PIN, 0);
    spiSend(cmd);
}

void adcWreg(int reg, int val)
{
    //see pages 40,43 of datasheet -
    /*spi_transaction_t t;
    char t_b[3];
    char r_b[3];
    t_b[0] = ADS129x::WREG | reg;
    t_b[1] = 0;
    t_b[2] = val;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=3*8;                   //Command is 3 x 8 bits
    t.tx_buffer= t_b;               //The data
    spi_device_polling_transmit(spi, &t);  //Transmit!    */
    spi_device_acquire_bus(spi, portMAX_DELAY);
    gpio_set_level(CS_PIN, 0); //from here it takes 10us to SCLK
    spiSend(ADS129x::WREG | reg); //time between bytes about 22us if bus aquired 12us
    ets_delay_us(2); //wait 2us
    spiSend(0);
    ets_delay_us(2); //wait 2us
    spiSend(val);
    gpio_set_level(CS_PIN, 1);
    spi_device_release_bus(spi);
    /*digitalWrite(PIN_CS, LOW);
    spiSend(ADS129x::WREG | reg);
    delayMicroseconds(2);
    spiSend(0);    // number of registers to be read/written – 1
    delayMicroseconds(2);
    spiSend(val);
    delayMicroseconds(1);
    digitalWrite(PIN_CS, HIGH);*/
}

int adcRreg(int reg)
{
    /*uint8_t out = 0;
    digitalWrite(PIN_CS, LOW);
    spiSend(ADS129x::RREG | reg);
    delayMicroseconds(2);
    spiSend(0);    // number of registers to be read/written – 1
    delayMicroseconds(2);
    out = spiRec();
    delayMicroseconds(1);
    digitalWrite(PIN_CS, HIGH);
    return ((int) out);*/
    uint8_t out = 0;
    
    spi_device_acquire_bus(spi, portMAX_DELAY);
    gpio_set_level(CS_PIN, 0);
    spiSend(ADS129x::RREG | reg);
    ets_delay_us(2); //wait 2us
    spiSend(0);
    ets_delay_us(2); //wait 2us
    out = spiRec();
    ets_delay_us(1); //wait 1us
    gpio_set_level(CS_PIN, 1);
    spi_device_release_bus(spi);
    return ((int)out);
}
