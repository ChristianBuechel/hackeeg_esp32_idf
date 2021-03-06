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
#include "esp_log.h"

#define TAG "adsCmd"
#define SPI_TRANSFER_SZ 64
spi_bus_config_t buscfg;
spi_device_interface_config_t devcfg;
spi_device_handle_t spi;

SemaphoreHandle_t xSemaphore = NULL;

volatile bool is_rdatac = false;
volatile bool is_rdata = false;
volatile uint32_t current_sample = 0;
volatile bool handling_data = false;
TaskHandle_t rdatac_task_handle = NULL;
TaskHandle_t read_task_handle = NULL;


uint8_t tx_data_NOP[SPI_TRANSFER_SZ] = {0}; //NOPs for receiving data

static void IRAM_ATTR drdy_interrupt(void *arg)
{
    static BaseType_t xHigherPriorityTaskWoken;
    xHigherPriorityTaskWoken = pdFALSE;
    if ((is_rdatac) | (is_rdata))//get ino ISR only in rdatac or rdata mode
    {
        //spi_data_available++;
        current_sample++; // increment even if there is a collison
        if (!handling_data) // means we have sent the last data
        {
            /*gpio_set_level(LED_PIN, 1);
            ets_delay_us(1);   // signal on scope
            gpio_set_level(LED_PIN, 0);*/
            //xSemaphoreGiveFromISR(xSemaphore, &xHigherPriorityTaskWoken); //tell rdatac task to run
            vTaskNotifyGiveFromISR(rdatac_task_handle, &xHigherPriorityTaskWoken);

            handling_data = true;
        }
        else
        {
            gpio_set_level(LED_PIN, 1);
            //ets_delay_us(1);   // signal collison on scope
            gpio_set_level(LED_PIN, 0);
        }
    }
    if (xHigherPriorityTaskWoken != pdFALSE)
    {
        // We can force a context switch here.  Context switching from an
        // ISR uses port specific syntax.  Check the demo task for your port
        // to find the syntax required.
        portYIELD_FROM_ISR();
    }
}

spi_transaction_t Rec_t;

void spi_init() //probably need to re-init when transfering data at hign speed
{
    //xSemaphore = xSemaphoreCreateBinary();
    // at the moment we handle the semaphore in the main loop
    buscfg.mosi_io_num = GPIO_NUM_23;
    buscfg.miso_io_num = GPIO_NUM_19;
    buscfg.sclk_io_num = GPIO_NUM_18;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = SPI_TRANSFER_SZ; // 64 is plenty

    devcfg.clock_speed_hz = 20 * 1000 * 1000; //Using 4 MHz mean we can send multibyte stuff in one go
                                              //in theory we can change that for data transfer
                                              //actually 16K SPS requires < 4 MHz
                                              //however that leaves not enough time to transmit over UART...

    devcfg.mode = 1; //SPI mode 1 p.12 CPOL = 0 and CPHA = 1.
    //devcfg.cs_ena_pretrans = 0;             //p.38 ADS1299 data sheet NOT needed if CS driven manaully
    //devcfg.cs_ena_posttrans = 0;            //p.38 ADS1299 data sheet NOT needed if CS driven manaully
    // 16 meesses up SPI, 4 works
    //devcfg.spics_io_num = CS_PIN;        //let esp operate CS pin
    devcfg.spics_io_num = -1; //we simply keep CS pin L
    devcfg.queue_size = 1;    //only one transactions at a time
    //devcfg.flags = SPI_DEVICE_HALFDUPLEX;    // try half duplex

    gpio_config_t gp;
    gp.intr_type = GPIO_INTR_DISABLE;
    gp.mode = GPIO_MODE_OUTPUT;
    gp.pull_up_en = GPIO_PULLUP_ENABLE;
    gp.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //gp.pin_bit_mask = (1ULL << RESET_PIN);
    gp.pin_bit_mask = (1ULL << CS_PIN) | (1ULL << RESET_PIN);
    // if you define this BEFORE starting SPI it is OK ...
    gpio_config(&gp);
    ESP_LOGI(TAG, "RESET_PIN init done");

    gp.pull_up_en = GPIO_PULLUP_DISABLE;
    gp.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gp.pin_bit_mask = (1ULL << CLKSEL_PIN) | (1ULL << START_PIN) | (1ULL << LED_PIN) | (1ULL << GPIO_NUM_19) | (1ULL << GPIO_NUM_23) | (1ULL << GPIO_NUM_18);
    gpio_config(&gp);
    ESP_LOGI(TAG, "CLKSEL START LED_PIN init done");

    gp.mode = GPIO_MODE_INPUT;
    gp.intr_type = GPIO_INTR_NEGEDGE;
    gp.pull_up_en = GPIO_PULLUP_ENABLE;
    gp.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gp.pin_bit_mask = 1ULL << DRDY_PIN;
    gpio_config(&gp);
    ESP_LOGI(TAG, "DRDY_PIN init done");

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(DRDY_PIN, drdy_interrupt, (void *)DRDY_PIN);
    ESP_LOGI(TAG, "DRDY_PIN ISR Installed");

    //startup p.62
    gpio_set_level(LED_PIN, 0); // LED off
    gpio_set_level(CLKSEL_PIN, 0); // use external clock
    //gpio_set_level(CLKSEL_PIN, 1); // use internal clock like hackeeg
    gpio_set_level(START_PIN, 1);  // start

    gpio_set_level(RESET_PIN, 1); // RESET H

    vTaskDelay(130 / portTICK_PERIOD_MS); // now wait 2^18 tCLK = 128ms
    gpio_set_level(RESET_PIN, 0);         // RESET !
    ets_delay_us(10);                     // >2 tCLK = 0.9 us
    gpio_set_level(RESET_PIN, 1);         // done

    gpio_set_level(START_PIN, 0); // control by command

    // we simply work w/o a CS pulse and keep line L
    gpio_set_level(CS_PIN, 0); // forever

    ESP_LOGI(TAG, "set various GPIOs");

    ESP_LOGI(TAG, "before spi_bus_initialize");
    spi_bus_initialize(VSPI_HOST, &buscfg, 0); //no DMA
    ESP_LOGI(TAG, "after spi_bus_initialize");
    spi_bus_add_device(VSPI_HOST, &devcfg, &spi);
    ESP_LOGI(TAG, "after spi_bus_add_device");

    spi_device_acquire_bus(spi, portMAX_DELAY); //could speed things up as we are the only customers

    // predefine makes no big difference
    memset(&Rec_t, 0, sizeof(Rec_t));
    Rec_t.tx_buffer = tx_data_NOP; // sending zeros !!
    Rec_t.flags = 0;
}

/** SPI receive a byte */
uint8_t spiRec()
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_data[0] = 0;
    t.flags = (SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA);
    spi_device_polling_transmit(spi, &t);
    return t.rx_data[0];
}

/** SPI receive multiple bytes */
uint8_t spiRec(uint8_t *buf, uint8_t len)
{
    // you could predefine the transaction to speed things up ...
    /*spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * len;
    t.rx_buffer = buf;
    t.tx_buffer = tx_data_NOP; // sending zeros !!
    t.flags = 0;
    spi_device_polling_transmit(spi, &t);*/
    //spi_device_transmit(spi, &t);

    Rec_t.length = 8 * len;
    Rec_t.rx_buffer = buf;
    spi_device_polling_transmit(spi, &Rec_t); //faster 52us @ 10 MHz
    //spi_device_polling_start(spi, &Rec_t, portMAX_DELAY); //faster 52us @ 10 MHz
    //spi_device_polling_end(spi, portMAX_DELAY);
    //spi_device_transmit(spi, &Rec_t); //very slow 86us @ 10 MHz
    return 0;
}

/** SPI send a byte */
void spiSend(uint8_t b)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t)); //Zero out the transaction
    t.length = 8;             //Command is 8 bits
    t.tx_data[0] = b;
    t.flags = SPI_TRANS_USE_TXDATA;
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

void adcSendCommand(uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t)); //Zero out the transaction
    t.length = 8;             //Command is 8 bits
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(spi, &t); //Transmit!
}

void adcWreg(uint8_t reg, uint8_t val)
{
    ESP_LOGI(TAG, "adcWreg");
    //see pages 40,43 of datasheet -
    //split up in 3 transfers to be able to use SCLK > 4 MHz
    spiSend(ADS129x::WREG | reg);
    spiSend(0);
    spiSend(val);

    //code below works upt to 4MHz SPI speed
    /*spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=3*8;                   //Command is 3 x 8 bits
    t.tx_data[0] = ADS129x::WREG | reg;
    t.tx_data[1] = 0;
    t.tx_data[2] = val;
    t.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(spi, &t);  //Transmit!
    */
}

uint8_t adcRreg(uint8_t reg)
{
    ESP_LOGI(TAG, "adcRreg");
    //split up in 3 transfers to be able to use SCLK > 4 MHz
    spiSend(ADS129x::RREG | reg);
    spiSend(0);
    return spiRec();

    //see pages 40,43 of datasheet -
    //code below works upt to 4MHz SPI speed
    /*spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=3*8;                   //Command is 3 x 8 bits
    t.tx_data[0] = ADS129x::RREG | reg;
    t.tx_data[1] = 0;
    t.tx_data[2] = 0;
    t.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
    spi_device_polling_transmit(spi, &t);  //Transmit!
    return  t.rx_data[2];*/
}
