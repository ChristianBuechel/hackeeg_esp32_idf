/* 
Port of the HACKEEG arduino program to the ESP32 IDF
CB
*/
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "ads129x.h"
#include "SerialCommand.h"
#include "JsonCommand.h"
#include "adsCommand.h"
#include "Base64.h"
#include "uart.h"
#include "driver/spi_master.h"

#define TAG "main"

#define TEXT_MODE 0
#define JSONLINES_MODE 1
#define MESSAGEPACK_MODE 2

#define SPI_BUFFER_SIZE 200    //max 27 bytes ...
#define OUTPUT_BUFFER_SIZE 512 //same here

const char *STATUS_TEXT_OK = "Ok";
const char *STATUS_TEXT_BAD_REQUEST = "Bad request";
const char *STATUS_TEXT_UNRECOGNIZED_COMMAND = "Unrecognized command";
const char *STATUS_TEXT_WRONG_REG = "Unrecognized register";
const char *STATUS_TEXT_WRONG_REG_VAL = "Unrecognized register value";
const char *STATUS_TEXT_ERROR = "Error";
const char *STATUS_TEXT_NOT_IMPLEMENTED = "Not Implemented";
const char *STATUS_TEXT_NO_ACTIVE_CHANNELS = "No Active Channels";

const char *hardware_type = "TI ADS1299 EVM";
const char *board_name = "ADS1299 EVM";
const char *maker_name = "Buchels";
const char *driver_version = "v0.1";

const char json_rdatac_header[] = "{\"C\":200,\"D\":\"";
uint8_t json_rdatac_header_size = sizeof(json_rdatac_header);
//uint8_t json_rdatac_header_size = sizeof (&json_rdatac_header[0]);

const char json_rdatac_footer[] = "\"}";
uint8_t json_rdatac_footer_size = sizeof(json_rdatac_footer);
//uint8_t json_rdatac_footer_size = sizeof (&json_rdatac_footer[0]);

const char messagepack_rdatac_header[] = {0x82, 0xa1, 0x43, 0xcc, 0xc8, 0xa1, 0x44, 0xc4};
uint8_t messagepack_rdatac_header_size = sizeof(messagepack_rdatac_header);

#define MP_HEADER_SZ 8
#define MP_DATA_SZ 27
#define MP_FULL_SZ 44 //8 + 4 + 4 + 1 + 27
#define MP_PRE_SZ 17  //MP_HEADER_SZ + 4 + 4 +1

union
{
    char bytes[MP_FULL_SZ];

    struct __attribute__((packed))
    {
        char header[MP_HEADER_SZ]; // messagepack header
        uint8_t size;              // size of cargo ie MP_DATA_SZ + 4 + 4
        uint32_t time;             // sample time
        uint32_t sample;           // sample #
        uint8_t data[MP_DATA_SZ];  // data ((8 ch + 1 status) x 3 bytes )
    } data_fields;

    struct __attribute__((packed))
    {
        char preSPI[MP_PRE_SZ];
        char postSPI[MP_DATA_SZ];
    } pre_post;

} mp_transfer;

#define SPI_FULL_SZ 35 // MP_DATA_SZ + 4 + 4

union
{
    char bytes[SPI_FULL_SZ];

    struct __attribute__((packed))
    {
        uint32_t time;            // sample time
        uint32_t sample;          // sample #
        uint8_t data[MP_DATA_SZ]; // data ((8 ch + 1 status) x 3 bytes )
    } data_fields;
} spi_transfer;

// do the same for b64 package anh hex package --> very nice ...
// but check whether data strings always have same length

//int protocol_mode = TEXT_MODE;
int protocol_mode = JSONLINES_MODE;

int max_channels = 0;
int num_active_channels = 0;
bool active_channels[9]; // reports whether channels 1..9 are active

int num_spi_bytes = 0;
int num_timestamped_spi_bytes = 0;

bool base64_mode = true;
int b64len = 0;
int hexlen = 0;

char hexDigits[] = "0123456789ABCDEF";

uint8_t spi_bytes[SPI_BUFFER_SIZE];

// char buffer to send via USB
char output_buffer[OUTPUT_BUFFER_SIZE];
char temp_buffer[OUTPUT_BUFFER_SIZE];

SerialCommand serialCommand; // The  SerialCommand object
JsonCommand jsonCommand;

extern "C"
{
    void app_main();
}

int hex_to_long(char *digits)
{
    using namespace std;
    char *error;
    int n = strtol(digits, &error, 16);
    if (*error != 0)
    {
        return -1; // error
    }
    else
    {
        return n;
    }
}

int encode_hex(char *output, char *input, int input_len)
{
    register int count = 0;
    for (register int i = 0; i < input_len; i++)
    {
        register uint8_t low_nybble = input[i] & 0x0f;
        register uint8_t highNybble = input[i] >> 4;
        output[count++] = hexDigits[highNybble];
        output[count++] = hexDigits[low_nybble];
    }
    output[count] = 0;
    return count;
}

void detectActiveChannels()
{
    if ((is_rdatac) || (max_channels < 1))
        return; //we can not read registers when in RDATAC mode
    using namespace ADS129x;
    num_active_channels = 0;
    for (int i = 1; i <= max_channels; i++)
    {
        ets_delay_us(1); //wait 1us
        int chSet = adcRreg(CHnSET + i);
        active_channels[i] = ((chSet & 7) != SHORTED);
        if ((chSet & 7) != SHORTED)
            num_active_channels++;
    }
}

void send_response(int status_code, const char *status_text)
{
    switch (protocol_mode)
    {
    case TEXT_MODE:
        printf("%d %s\n", status_code, status_text);
        break;
    case JSONLINES_MODE:
        jsonCommand.sendJsonLinesResponse(status_code, (char *)status_text);
        break;
    case MESSAGEPACK_MODE:
        // all responses are in JSON Lines, MessagePack mode is only for sending samples
        jsonCommand.sendJsonLinesResponse(status_code, (char *)status_text);
        break;
    default:
        // unknown protocol
        ;
    }
}

void send_response_ok()
{
    send_response(RESPONSE_OK, STATUS_TEXT_OK);
}

void send_response_error()
{
    send_response(RESPONSE_ERROR, STATUS_TEXT_ERROR);
}

// This gets set as the default handler, and gets called when no other command matches.
void unrecognized(const char *command)
{
    //Serial.println("406 Error: Unrecognized command.");
    //Serial.println();
    printf("406 Error: Unrecognized command.\n");
    ESP_LOGI(TAG, "%s\n", command);
}
// This gets set as the default handler for jsonlines and messagepack, and gets called when no other command matches.
void unrecognizedJsonLines(const char *command)
{
    /*StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root[STATUS_CODE_KEY] = UNRECOGNIZED_COMMAND;
    root[STATUS_TEXT_KEY] = "Unrecognized command";
    jsonCommand.sendJsonLinesDocResponse(doc);*/

    cJSON *root;
    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(RESPONSE_UNRECOGNIZED_COMMAND));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString("Unrecognized command"));

    /*char* text = cJSON_Print(root);
    printf("%s\n", text);
    free(text);
    cJSON_Delete(root);*/
    jsonCommand.sendJsonLinesDocResponse(root); //als Alternative
}

void adsSetup()
{ //default settings for ADS1298 and compatible chips
    using namespace ADS129x;
    // Send SDATAC Command (Stop Read Data Continuously mode)
    //spi_data_available = 0;

    current_sample = 0;
    handling_data = false;

    adcSendCommand(SDATAC);
    ESP_LOGI(TAG, "sent SDATAC");
    //vTaskDelay(100 / portTICK_PERIOD_MS);
    //ets_delay_us(2);
    //delay(100);
    uint8_t val = adcRreg(ID);
    ESP_LOGI(TAG, "ID = %d", val);
    switch (val & DEV_ID_MASK)
    {
    case (DEV_ID_MASK_129x | ID_4CHAN):
        hardware_type = "ADS1294";
        max_channels = 4;
        break;
    //case B10001:
    case (DEV_ID_MASK_129x | ID_6CHAN):
        hardware_type = "ADS1296";
        max_channels = 6;
        break;
    //case B10010:
    case (DEV_ID_MASK_129x | ID_8CHAN):
        hardware_type = "ADS1298";
        max_channels = 8;
        break;
    //case B11110:
    case (DEV_ID_MASK_1299 | ID_8CHAN):
        hardware_type = "ADS1299";
        max_channels = 8;
        break;
    //case B11100:
    case (DEV_ID_MASK_1299 | ID_4CHAN):
        hardware_type = "ADS1299-4";
        max_channels = 4;
        break;
    //case B11101:
    case (DEV_ID_MASK_1299 | ID_6CHAN):
        hardware_type = "ADS1299-6";
        max_channels = 6;
        break;
    default:
        max_channels = 0;
    }
    if (max_channels == 0)
    { //error mode
        while (1)
        {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    } //error mode

    // All GPIO set to output 0x0000: (floating CMOS inputs can flicker on and off, creating noise)
    adcWreg(ADS_GPIO, 0);
    adcWreg(CONFIG3, PD_REFBUF | CONFIG3_const);
    //gpio_set_level(START_PIN, 1); //Hmmm not sure ??? should be L to use commands ...
    //gpio_set_level(START_PIN, 0); //L to use commands ...
}

void nopCommand(unsigned char unused1, unsigned char unused2)
{
    send_response_ok();
}

void microsCommand(unsigned char unused1, unsigned char unused2)
{
    int64_t microseconds = esp_timer_get_time();
    if (protocol_mode == TEXT_MODE)
    {
        send_response_ok();
        printf("%" PRId64 "\n", microseconds);
        return;
    }

    cJSON *root;
    ESP_LOGI(TAG, "Main Free Heap before root %d", esp_get_free_heap_size());
    root = cJSON_CreateObject();
    ESP_LOGI(TAG, "Main Free Heap after root %d", esp_get_free_heap_size());
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
    cJSON_AddItemToObject(root, DATA_KEY, cJSON_CreateNumber(microseconds));

    switch (protocol_mode)
    {
    case JSONLINES_MODE:
    case MESSAGEPACK_MODE:
        ESP_LOGI(TAG, "Main Free Heap before sendJsonLinesDocResponse %d", esp_get_free_heap_size());
        jsonCommand.sendJsonLinesDocResponse(root);
        break;
    default:
        // unknown protocol
        ;
    }
}
void versionCommand(unsigned char unused1, unsigned char unused2)
{
    send_response(RESPONSE_OK, driver_version);
}

void statusCommand(unsigned char unused1, unsigned char unused2)
{
    detectActiveChannels();
    if (protocol_mode == TEXT_MODE)
    {
        printf("200 Ok\n");
        printf("Driver version: %s\n", driver_version);
        printf("Board name: %s\n", board_name);
        printf("Board maker: %s\n", maker_name);
        printf("Hardware type: %s\n", hardware_type);
        printf("Max channels: %d\n", max_channels);
        printf("Number of active channels: %d\n\n", num_active_channels);
        return;
    }

    cJSON *root, *cj_data;
    ESP_LOGI(TAG, "Main Free Heap before root %d", esp_get_free_heap_size());
    root = cJSON_CreateObject();
    ESP_LOGI(TAG, "Main Free Heap after root %d", esp_get_free_heap_size());
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
    cJSON_AddItemToObject(root, DATA_KEY, cj_data = cJSON_CreateObject());
    cJSON_AddStringToObject(cj_data, "driver_version", driver_version);
    cJSON_AddStringToObject(cj_data, "board_name", board_name);
    cJSON_AddStringToObject(cj_data, "maker_name", maker_name);
    cJSON_AddStringToObject(cj_data, "hardware_type", hardware_type);
    cJSON_AddNumberToObject(cj_data, "max_channels", max_channels);
    cJSON_AddNumberToObject(cj_data, "active_channels", num_active_channels);

    switch (protocol_mode)
    {
    case JSONLINES_MODE:
    case MESSAGEPACK_MODE:
        jsonCommand.sendJsonLinesDocResponse(root);
        break;
    default:
        // unknown protocol
        ;
    }
}

void serialNumberCommand(unsigned char unused1, unsigned char unused2)
{
    send_response(RESPONSE_NOT_IMPLEMENTED, STATUS_TEXT_NOT_IMPLEMENTED);
}

void textCommand(unsigned char unused1, unsigned char unused2)
{
    protocol_mode = TEXT_MODE;
    send_response_ok();
}

void jsonlinesCommand(unsigned char unused1, unsigned char unused2)
{
    protocol_mode = JSONLINES_MODE;
    send_response_ok();
}

void messagepackCommand(unsigned char unused1, unsigned char unused2)
{
    protocol_mode = MESSAGEPACK_MODE;
    send_response_ok();
}

void ledOnCommand(unsigned char unused1, unsigned char unused2)
{
    gpio_set_level(LED_PIN, 1);
    send_response_ok();
}

void ledOffCommand(unsigned char unused1, unsigned char unused2)
{
    gpio_set_level(LED_PIN, 0);
    send_response_ok();
}

void boardLedOnCommand(unsigned char unused1, unsigned char unused2)
{
    uint8_t state = adcRreg(ADS129x::ADS_GPIO);
    //state = state & 0xF7; --> was all done for GPIO4 we now use GPIO1
    state = state & ~ADS129x::GPIO_bits::GPIOC1; //set pin to OUTPUT
    //state = state | 0x80;
    state = state | ADS129x::GPIO_bits::GPIOD1; //set GPIO Pin

    adcWreg(ADS129x::ADS_GPIO, state);

    send_response_ok();
}

void boardLedOffCommand(unsigned char unused1, unsigned char unused2)
{
    uint8_t state = adcRreg(ADS129x::ADS_GPIO);
    ESP_LOGI(TAG, "State after read %#x", state);
    //state = state & 0x77;
    state = state & ~(ADS129x::GPIO_bits::GPIOC1 | ADS129x::GPIO_bits::GPIOD1);
    ESP_LOGI(TAG, "State before write %#x", state);
    adcWreg(ADS129x::ADS_GPIO, state);

    send_response_ok();
}

void wakeupCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(WAKEUP);
    send_response_ok();
}

void standbyCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(STANDBY);
    send_response_ok();
}

void resetCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(RESET);
    vTaskDelay(150 / portTICK_PERIOD_MS); //now wait 2^18 tCLK = 128ms
    adsSetup();
    send_response_ok();
}

void startCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(START);
    current_sample = 0;
    send_response_ok();
}

void stopCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(STOP);
    send_response_ok();
}

void rdatacCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    detectActiveChannels();
    if (num_active_channels > 0)
    {
        adcSendCommand(RDATAC);
        send_response_ok();
        handling_data = false; //fresh start
        current_sample = 0;    //here or whe start commad is issued?
        is_rdatac = true;      //now ISR is armed ...
    }
    else
    {
        send_response(RESPONSE_NO_ACTIVE_CHANNELS, STATUS_TEXT_NO_ACTIVE_CHANNELS);
    }
}

void rdataCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    detectActiveChannels();
    if (num_active_channels > 0)
    {
        send_response_ok();
        handling_data = false; //fresh start
        current_sample = 0;    //here or whe start commad is issued?
        is_rdata = true;       //now ISR is armed ...
    }
    else
    {
        send_response(RESPONSE_NO_ACTIVE_CHANNELS, STATUS_TEXT_NO_ACTIVE_CHANNELS);
    }
}

void sdatacCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    is_rdatac = false;
    adcSendCommand(SDATAC);
    using namespace ADS129x;
    send_response_ok();
}

/*void rdataCommand(unsigned char unused1, unsigned char unused2) // TBD
{
    using namespace ADS129x;
    while (gpio_get_level(DRDY_PIN) == 1)
        ; //wdt kicks in ...
    adcSendCommand(RDATA);
    if (protocol_mode == TEXT_MODE)
    {
        send_response_ok();
    }
    //send_sample(); TBD --> not implemented ...

}
*/
void readRegisterCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    char *arg1;
    arg1 = serialCommand.next();
    if (arg1 != NULL)
    {
        int registerNumber = hex_to_long(arg1);
        //if (registerNumber >= 0)
        if (registerNumber >= 0x00 && registerNumber <= 0xff)
        {
            int result = adcRreg(registerNumber);
            printf("200 Ok (Read Register %#x)\n%#x\n", registerNumber, result);
        }
        else
        {
            printf("402 Error: expected hexidecimal digits.\n");
        }
    }
    else
    {
        printf("403 Error: register argument missing.\n");
    }
    printf("\n");
}

void writeRegisterCommand(unsigned char unused1, unsigned char unused2)
{
    char *arg1, *arg2;
    arg1 = serialCommand.next();
    arg2 = serialCommand.next();
    if (arg1 != NULL)
    {
        if (arg2 != NULL)
        {
            int registerNumber = hex_to_long(arg1);
            int registerValue = hex_to_long(arg2);
            if (registerNumber >= 0 && registerValue >= 0)
            {
                adcWreg(registerNumber, registerValue);
                printf("200 Ok (Write Register %#x %#x)\n", registerNumber, registerValue);
            }
            else
            {
                printf("402 Error: expected hexadecimal digits.\n");
            }
        }
        else
        {
            printf("404 Error: value argument missing.\n");
        }
    }
    else
    {
        printf("403 Error: register argument missing.\n");
    }
    printf("\n");
}

void readRegisterCommandDirect(unsigned char register_number, unsigned char unused1)
{
    using namespace ADS129x;
    /*   if (register_number >= 0 and register_number <= 255)
    // this needs to checked in JSON decoding !!!
    {
     */
    unsigned char result = adcRreg(register_number);
    cJSON *root;
    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
    cJSON_AddItemToObject(root, DATA_KEY, cJSON_CreateNumber(result));
    jsonCommand.sendJsonLinesDocResponse(root);
    /* }
    else
    {
        send_response_error();
    } */
}

void writeRegisterCommandDirect(unsigned char register_number, unsigned char register_value)
{
    /*    if (register_number >= 0 && register_value >= 0)
    // this needs to checked in JSON decoding !!!
    {  */
    adcWreg(register_number, register_value);
    send_response_ok();
    /*}
    else
    {
        send_response_error();
    }*/
}

void base64ModeOnCommand(unsigned char unused1, unsigned char unused2)
{
    base64_mode = true;
    send_response(RESPONSE_OK, "Base64 mode on - rdata command will respond with base64 encoded data.");
}

void hexModeOnCommand(unsigned char unused1, unsigned char unused2)
{
    base64_mode = false;
    send_response(RESPONSE_OK, "Hex mode on - rdata command will respond with hex encoded data");
}

void testCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(START);
    current_sample = 0;
    adcWreg(ADS129x::CONFIG2, (CONFIG2_const | INT_TEST_4HZ));
    adcWreg(ADS129x::CH1SET, TEST_SIGNAL);
    adcWreg(ADS129x::CH2SET, SHORTED);
    adcWreg(ADS129x::CH3SET, TEST_SIGNAL);
    adcWreg(ADS129x::CH4SET, SHORTED);
    adcWreg(ADS129x::CH5SET, TEMP);
    is_rdatac = true;
    adcSendCommand(RDATAC);

    send_response(RESPONSE_OK, "Test - Square on ch 1 and ch 3");
}

void helpCommand(unsigned char unused1, unsigned char unused2)
{
    if (protocol_mode == JSONLINES_MODE || protocol_mode == MESSAGEPACK_MODE)
    {
        send_response(RESPONSE_OK, "Help not available in JSON Lines or MessagePack modes.");
    }
    else
    {
        printf("200 Ok\n");
        printf("Available commands: \n");
        serialCommand.printCommands();
        printf("\n");
    }
}

static void rdatac_task(void *arg)
{
    memcpy(mp_transfer.data_fields.header, messagepack_rdatac_header, MP_HEADER_SZ);
    // setup header bytes
    mp_transfer.data_fields.size = MP_DATA_SZ + 4 + 4;
    // setup size (data + counter + time )

    while (1)
    {
        // wait for ISR to wake us ...
        //if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) //saves 1us ;-)
        {
            /*gpio_set_level(LED_PIN, 1);
            ets_delay_us(1); // signal collison on scope
            gpio_set_level(LED_PIN, 0);*/

            if (is_rdata) //ask for data
            {
                using namespace ADS129x;
                adcSendCommand(RDATA);
                is_rdata = false; // just one conversion
            }
            switch (protocol_mode)
            {
            case MESSAGEPACK_MODE:
            {
                //this should be pretty fast
                mp_transfer.data_fields.time = esp_timer_get_time(); //cave 64bit
                mp_transfer.data_fields.sample = current_sample;
                //now transmit the first chunk
                uart_write(mp_transfer.pre_post.preSPI, MP_PRE_SZ);
                spiRec(mp_transfer.data_fields.data, MP_DATA_SZ);
                //...and second chunk
                uart_write(mp_transfer.pre_post.postSPI, MP_DATA_SZ);

                //uart_write(mp_transfer.bytes, MP_FULL_SZ); //the whole lot
                /*gpio_set_level(LED_PIN, 1);
            ets_delay_us(2); //wait 2us
            gpio_set_level(LED_PIN, 0);*/
            }
            break;

            case JSONLINES_MODE:
            {
                spi_transfer.data_fields.time = esp_timer_get_time(); //cave 64bit
                spi_transfer.data_fields.sample = current_sample;
                spiRec(spi_transfer.data_fields.data, MP_DATA_SZ);

                b64len = base64_encode(temp_buffer, spi_transfer.bytes, SPI_FULL_SZ);
                size_t count = 0;
                memcpy(&output_buffer[count], json_rdatac_header, json_rdatac_header_size);
                count += json_rdatac_header_size;
                //memcpy(&output_buffer[count], json_rdatac_header, 14);
                //count += 14;

                memcpy(&output_buffer[count], temp_buffer, b64len);
                count += b64len;

                memcpy(&output_buffer[count], json_rdatac_footer, json_rdatac_footer_size);
                count += json_rdatac_footer_size;
                //memcpy(&output_buffer[count], json_rdatac_footer, 2);
                //count += 2;
                output_buffer[count++] = 0x0a;
                uart_write((char *)output_buffer, count);
            }
            break;

            case TEXT_MODE:
            {
                spi_transfer.data_fields.time = esp_timer_get_time(); //cave 64bit
                spi_transfer.data_fields.sample = current_sample;
                spiRec(spi_transfer.data_fields.data, MP_DATA_SZ);
                if (base64_mode)
                {
                    b64len = base64_encode(output_buffer, spi_transfer.bytes, SPI_FULL_SZ);
                }
                else
                {
                    b64len = encode_hex(output_buffer, spi_transfer.bytes, SPI_FULL_SZ);
                }
                output_buffer[b64len++] = 0x0a; //add newline
                uart_write((char *)output_buffer, b64len);
            }
            break;

            default:
                break;
            }
            handling_data = false; //we are done
        }
    }
}

static void read_task(void *arg) //task checking the UART for commands
{
    while (1)
    {
        switch (protocol_mode)
        {
        case TEXT_MODE:
            serialCommand.readSerial();
            break;
        case JSONLINES_MODE:
        case MESSAGEPACK_MODE:
            jsonCommand.readSerial();
            break;
        default:
            // do nothing
            ;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS); //UART input gets handled every 10 ms ...
    }
}

void app_main(void)
{

    /*Bootloader config
Bootloader log verbosity
-->No output

Log output
Default log verbosity
-->No output*/
    //esp_log_level_set("*", ESP_LOG_INFO); //todo change by command
    vTaskDelay(500 / portTICK_PERIOD_MS); //wait --> see whether this is OK*/
    esp_log_level_set("*", ESP_LOG_NONE); //todo change by command
    ESP_LOGI(TAG, "Hi");
    uart_init();
    protocol_mode = TEXT_MODE;
    ESP_LOGI(TAG, "UART initialized");
    spi_init(); //start SPI, define semaphore, do GPIO stuff
    ESP_LOGI(TAG, "SPI initialized");
    adsSetup();
    ESP_LOGI(TAG, "ADS1299 initialized");

    xSemaphore = xSemaphoreCreateBinary();                                                      //not neede anymore
    xTaskCreatePinnedToCore(rdatac_task, "rdatac_task", 4096, NULL, 5, &rdatac_task_handle, 0); //params?? prio 2 ??

    serialCommand.setDefaultHandler(unrecognized);                 //
    serialCommand.addCommand("nop", nopCommand);                   // No operation (does nothing)
    serialCommand.addCommand("micros", microsCommand);             // Returns number of microseconds since the program began executing
    serialCommand.addCommand("version", versionCommand);           // Echos the driver version number
    serialCommand.addCommand("status", statusCommand);             // Echos the driver status
    serialCommand.addCommand("serialnumber", serialNumberCommand); // Echos the board serial number (UUID from the onboard 24AA256UID-I/SN I2S EEPROM)
    serialCommand.addCommand("text", textCommand);                 // Sets the communication protocol to text
    serialCommand.addCommand("jsonlines", jsonlinesCommand);       // Sets the communication protocol to JSONLines
    serialCommand.addCommand("messagepack", messagepackCommand);   // Sets the communication protocol to MessagePack
    serialCommand.addCommand("ledon", ledOnCommand);               // Turns ESP32 LED (if connected) on
    serialCommand.addCommand("ledoff", ledOffCommand);             // Turns ESP32 LED off
    serialCommand.addCommand("boardledon", boardLedOnCommand);     // Turns ADS1299 GPIO1 LED on
    serialCommand.addCommand("boardledoff", boardLedOffCommand);   // Turns ADS1299 GPIO1 LED off
    serialCommand.addCommand("wakeup", wakeupCommand);             // Send the WAKEUP command
    serialCommand.addCommand("standby", standbyCommand);           // Send the STANDBY command
    serialCommand.addCommand("reset", resetCommand);               // Reset the ADS1299
    serialCommand.addCommand("start", startCommand);               // Send START command
    serialCommand.addCommand("stop", stopCommand);                 // Send STOP command
    serialCommand.addCommand("rdatac", rdatacCommand);             // Enter read data continuous mode, clear the ringbuffer, and read new data into the ringbuffer
    serialCommand.addCommand("sdatac", sdatacCommand);             // Stop read data continuous mode; ringbuffer data is still available
    serialCommand.addCommand("rdata", rdataCommand);               // Read one sample of data from each active channel
    serialCommand.addCommand("rreg", readRegisterCommand);         // Read ADS129x register, argument in hex, print contents in hex
    serialCommand.addCommand("wreg", writeRegisterCommand);        // Write ADS129x register, arguments in hex
    serialCommand.addCommand("base64", base64ModeOnCommand);       // RDATA commands send base64 encoded data - default
    serialCommand.addCommand("hex", hexModeOnCommand);             // RDATA commands send hex encoded data
    serialCommand.addCommand("test", testCommand);                 // set to square wave enable ch 1 and 3
    serialCommand.addCommand("help", helpCommand);                 // Print list of commands
    serialCommand.clearBuffer();

    jsonCommand.setDefaultHandler(unrecognizedJsonLines);        // Handler for any command that isn't matched
    jsonCommand.addCommand("nop", nopCommand);                   // No operation (does nothing)
    jsonCommand.addCommand("micros", microsCommand);             // Returns number of microseconds since the program began executing
    jsonCommand.addCommand("version", versionCommand);           // Echos the driver version number
    jsonCommand.addCommand("status", statusCommand);             // Echos the driver status
    jsonCommand.addCommand("serialnumber", serialNumberCommand); // Echos the board serial number (UUID from the onboard 24AA256UID-I/SN I2S EEPROM)
    jsonCommand.addCommand("text", textCommand);                 // Sets the communication protocol to text
    jsonCommand.addCommand("jsonlines", jsonlinesCommand);       // Sets the communication protocol to JSONLines
    jsonCommand.addCommand("messagepack", messagepackCommand);   // Sets the communication protocol to MessagePack
    jsonCommand.addCommand("ledon", ledOnCommand);               // Turns Arduino Due onboard LED on
    jsonCommand.addCommand("ledoff", ledOffCommand);             // Turns Arduino Due onboard LED off
    jsonCommand.addCommand("boardledon", boardLedOnCommand);     // Turns ADS1299 GPIO1 LED on
    jsonCommand.addCommand("boardledoff", boardLedOffCommand);   // Turns ADS1299 GPIO1 LED off
    jsonCommand.addCommand("wakeup", wakeupCommand);             // Send the WAKEUP command
    jsonCommand.addCommand("standby", standbyCommand);           // Send the STANDBY command
    jsonCommand.addCommand("reset", resetCommand);               // Reset the ADS1299
    jsonCommand.addCommand("start", startCommand);               // Send START command
    jsonCommand.addCommand("stop", stopCommand);                 // Send STOP command
    jsonCommand.addCommand("rdatac", rdatacCommand);             // Enter read data continuous mode, clear the ringbuffer, and read new data into the ringbuffer
    jsonCommand.addCommand("sdatac", sdatacCommand);             // Stop read data continuous mode; ringbuffer data is still available
    jsonCommand.addCommand("rdata", rdataCommand);               // Read one sample of data from each active channel
    jsonCommand.addCommand("rreg", readRegisterCommandDirect);   // Read ADS129x register, argument in hex, print contents in hex
    jsonCommand.addCommand("wreg", writeRegisterCommandDirect);  // Write ADS129x register, arguments in hex
    jsonCommand.addCommand("help", helpCommand);                 // Print list of commands
    jsonCommand.clearBuffer();
    xTaskCreatePinnedToCore(read_task, "read_task", 4096, NULL, 1, &read_task_handle, 1); //params?? prio 2 ??
    /*while (1) //main loop
    {
        switch (protocol_mode)
        {
        case TEXT_MODE:
            serialCommand.readSerial();
            break;
        case JSONLINES_MODE:
        case MESSAGEPACK_MODE:
            jsonCommand.readSerial();
            break;
        default:
            // do nothing
            ;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS); //UART input gets handled every 10 ms ...
    }*/
}
