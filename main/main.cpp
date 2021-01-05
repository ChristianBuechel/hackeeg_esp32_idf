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
const char *STATUS_TEXT_ERROR = "Error";
const char *STATUS_TEXT_NOT_IMPLEMENTED = "Not Implemented";
const char *STATUS_TEXT_NO_ACTIVE_CHANNELS = "No Active Channels";

const char *hardware_type = "TI ADS1299 EVM";
const char *board_name = "ADS1299 EVM";
const char *maker_name = "Buchels";
const char *driver_version = "v0.1";

const char *json_rdatac_header = "{\"C\":200,\"D\":\"";
uint8_t json_rdatac_header_size = sizeof(json_rdatac_header);
const char *json_rdatac_footer = "\"}";
uint8_t json_rdatac_footer_size = sizeof(json_rdatac_footer);

const char messagepack_rdatac_header[] = {0x82, 0xa1, 0x43, 0xcc, 0xc8, 0xa1, 0x44, 0xc4};
uint8_t messagepack_rdatac_header_size = sizeof(messagepack_rdatac_header);

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

// microseconds timestamp
#define TIMESTAMP_SIZE_IN_BYTES 4
union
{
    char timestamp_bytes[TIMESTAMP_SIZE_IN_BYTES];
    unsigned long timestamp;
} timestamp_union;

// sample number counter
#define SAMPLE_NUMBER_SIZE_IN_BYTES 4
union
{
    char sample_number_bytes[SAMPLE_NUMBER_SIZE_IN_BYTES];
    unsigned long sample_number = 0;
} sample_number_union;

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
{ //set device into RDATAC (continous) mode -it will stream data
    if ((is_rdatac) || (max_channels < 1))
        return; //we can not read registers when in RDATAC mode
    //Serial.println("Detect active channels: ");
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

inline void receive_sample()
{ //inline necessary ??
    gpio_set_level(CS_PIN, 0);
    ets_delay_us(10); //wait 10us DO WE NEED THIS ??
    memset(spi_bytes, 0, sizeof(spi_bytes));
    timestamp_union.timestamp = esp_timer_get_time(); //cave 64bit
    spi_bytes[0] = timestamp_union.timestamp_bytes[0];
    spi_bytes[1] = timestamp_union.timestamp_bytes[1];
    spi_bytes[2] = timestamp_union.timestamp_bytes[2];
    spi_bytes[3] = timestamp_union.timestamp_bytes[3];
    spi_bytes[4] = sample_number_union.sample_number_bytes[0];
    spi_bytes[5] = sample_number_union.sample_number_bytes[1];
    spi_bytes[6] = sample_number_union.sample_number_bytes[2];
    spi_bytes[7] = sample_number_union.sample_number_bytes[3];

    uint8_t returnCode = spiRec(spi_bytes + TIMESTAMP_SIZE_IN_BYTES + SAMPLE_NUMBER_SIZE_IN_BYTES, num_spi_bytes);
    gpio_set_level(CS_PIN, 1);
    sample_number_union.sample_number++;
}

inline void send_sample_messagepack(int num_bytes)
{
    /*uart_write((char *)messagepack_rdatac_header, messagepack_rdatac_header_size);
    uart_write((char *)&num_bytes, 1);
    uart_write((char *)spi_bytes, num_bytes);*/

    size_t count = 0;
    memcpy(&output_buffer[count], messagepack_rdatac_header, messagepack_rdatac_header_size);
    count += messagepack_rdatac_header_size;
    output_buffer[count++] = (char)num_bytes;
    memcpy(&output_buffer[count], spi_bytes, num_bytes);
    count += num_bytes;
    output_buffer[count++] = 0x0a;
    uart_write((char *)output_buffer, count);
}

inline void send_sample(void)
{
    switch (protocol_mode)
    {
    case JSONLINES_MODE:
        /*printf("%s", json_rdatac_header);
        base64_encode(output_buffer, (char *)spi_bytes, num_timestamped_spi_bytes);
        printf("%s", output_buffer);
        printf("%s\n", json_rdatac_footer);
        break;*/
        {
            b64len = base64_encode(temp_buffer, (char *)spi_bytes, num_timestamped_spi_bytes);
            size_t count = 0;
            memcpy(&output_buffer[count], json_rdatac_header, json_rdatac_header_size);
            count += json_rdatac_header_size;
            memcpy(&output_buffer[count], temp_buffer, b64len);
            count += b64len;
            memcpy(&output_buffer[count], json_rdatac_footer, json_rdatac_footer_size);
            count += json_rdatac_footer_size;
            output_buffer[count++] = 0x0a;
            uart_write((char *)output_buffer, count);
        }
        break;
    case TEXT_MODE:
        if (base64_mode)
        {
            b64len = base64_encode(output_buffer, (char *)spi_bytes, num_timestamped_spi_bytes);
        }
        else
        {
            b64len = encode_hex(output_buffer, (char *)spi_bytes, num_timestamped_spi_bytes);
        }
        //printf("%s\n", output_buffer);
        output_buffer[b64len++] = 0x0a; //add newline
        uart_write((char *)output_buffer, b64len);

        break;
    case MESSAGEPACK_MODE:
        send_sample_messagepack(num_timestamped_spi_bytes);
        break;
    }
}

inline void send_sample_json(int num_bytes)
{
    cJSON *root;
    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
    cJSON_AddItemToObject(root, DATA_KEY, cJSON_CreateIntArray((const int *)spi_bytes, num_bytes));
    jsonCommand.sendJsonLinesDocResponse(root);
}

inline void send_samples(void)
{
    if (!is_rdatac)
        return;
    if (spi_data_available)
    {
        spi_data_available = 0;
        receive_sample();
        send_sample();
    }
}

void adsSetup()
{ //default settings for ADS1298 and compatible chips
    using namespace ADS129x;
    // Send SDATAC Command (Stop Read Data Continuously mode)
    spi_data_available = 0;
    //attachInterrupt(digitalPinToInterrupt(IPIN_DRDY), drdy_interrupt, FALLING); done in spi_init
    adcSendCommand(SDATAC);

    //vTaskDelay(1000 / portTICK_PERIOD_MS);

    // delayMicroseconds(2);
    //delay(100);
    int val = adcRreg(ID);
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
    num_spi_bytes = (3 * (max_channels + 1)); //24-bits header plus 24-bits per channel
    num_timestamped_spi_bytes = num_spi_bytes + TIMESTAMP_SIZE_IN_BYTES + SAMPLE_NUMBER_SIZE_IN_BYTES;
    if (max_channels == 0)
    { //error mode
        while (1)
        {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
    } //error mode

    // All GPIO set to output 0x0000: (floating CMOS inputs can flicker on and off, creating noise)
    adcWreg(ADS_GPIO, 0);
    adcWreg(CONFIG3, PD_REFBUF | CONFIG3_const);
    //gpio_set_level(START_PIN, 1); //Hmmm not sure ??? should be L to use commands ...
    gpio_set_level(START_PIN, 0); //L to use commands ...
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
        //Serial.println(microseconds);
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
    //printf("%s\n", cJSON_Print(root));
    //cJSON_Delete(root);
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
    int state = adcRreg(ADS129x::ADS_GPIO);
    ESP_LOGI(TAG, "State after read %#x", state);
    //state = state & 0xF7; --> was all done for GPIO4 we now use GPIO1
    state = state & ~ADS129x::GPIO_bits::GPIOC1; //set pin to OUTPUT
    //state = state | 0x80;
    state = state | ADS129x::GPIO_bits::GPIOD1; //set GPIO Pin
    ESP_LOGI(TAG, "State before write %#x", state);
    adcWreg(ADS129x::ADS_GPIO, state);

    send_response_ok();
}

void boardLedOffCommand(unsigned char unused1, unsigned char unused2)
{
    int state = adcRreg(ADS129x::ADS_GPIO);
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
    send_response_ok();
    adsSetup();
}

void startCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    adcSendCommand(START);
    sample_number_union.sample_number = 0;
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
        is_rdatac = true;
        adcSendCommand(RDATAC);
        send_response_ok();
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

void rdataCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    while (gpio_get_level(DRDY_PIN) == 1)
        ; //wdt kicks in ...
    adcSendCommandLeaveCsActive(RDATA);
    if (protocol_mode == TEXT_MODE)
    {
        send_response_ok();
    }
    send_sample();
}

void readRegisterCommand(unsigned char unused1, unsigned char unused2)
{
    using namespace ADS129x;
    char *arg1;
    arg1 = serialCommand.next();
    if (arg1 != NULL)
    {
        int registerNumber = hex_to_long(arg1);
        if (registerNumber >= 0)
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
    if (register_number >= 0 and register_number <= 255)
    {
        unsigned char result = adcRreg(register_number);

        cJSON *root;
        root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
        cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
        cJSON_AddItemToObject(root, DATA_KEY, cJSON_CreateNumber(result));
        jsonCommand.sendJsonLinesDocResponse(root);
    }
    else
    {
        send_response_error();
    }
}

void writeRegisterCommandDirect(unsigned char register_number, unsigned char register_value)
{
    if (register_number >= 0 && register_value >= 0)
    {
        adcWreg(register_number, register_value);
        send_response_ok();
    }
    else
    {
        send_response_error();
    }
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
    sample_number_union.sample_number = 0;

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

void app_main(void)
{
    /*Bootloader config
Bootloader log verbosity
-->No output

Log output
Default log verbosity
-->No output*/
    //esp_log_level_set("*", ESP_LOG_INFO); //todo change by command
    esp_log_level_set("*", ESP_LOG_NONE); //todo change by command
    ESP_LOGI(TAG, "Hi");
    uart_init();
    protocol_mode = TEXT_MODE;
    //protocol_mode = JSONLINES_MODE;
    ESP_LOGI(TAG, "UART initialized");
    spi_init();
    ESP_LOGI(TAG, "SPI initialized");
    adsSetup();
    ESP_LOGI(TAG, "ADS1299 initialized");

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
    while (1) //main loop
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

        if (xSemaphoreTake(xSemaphore, 10 / portTICK_PERIOD_MS) == pdTRUE) //read uart every 10 ms
        {
            send_samples();
            }

        /*send_samples();
        vTaskDelay(10 / portTICK_PERIOD_MS); //wait --> see whether this is OK*/
    }
}
