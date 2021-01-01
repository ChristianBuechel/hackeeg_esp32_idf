/* 
Port of the HACKEEG arduino program to the ESP32 IDF
CB
*/
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "ads129x.h"
#include "SerialCommand.h"
#include "JsonCommand.h"
#include "adsCommand.h"
#include "uart.h"
#include "driver/spi_master.h"

#define TAG "main"

#define TEXT_MODE 0
#define JSONLINES_MODE 1
#define MESSAGEPACK_MODE 2

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

int protocol_mode = TEXT_MODE;
//int protocol_mode = JSONLINES_MODE;

int max_channels = 0;
int num_active_channels = 0;
bool active_channels[9]; // reports whether channels 1..9 are active
bool is_rdatac = false;
bool base64_mode = true;

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

int hex_to_long(char *digits) {
    using namespace std;
    char *error;
    int n = strtol(digits, &error, 16);
    if (*error != 0) {
        return -1; // error
    } else {
        return n;
    }
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

void send_response_error() {
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
    //adsSetup(); TBD
    send_response_ok();
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
        ;
    adcSendCommandLeaveCsActive(RDATA);
    if (protocol_mode == TEXT_MODE)
    {
        send_response_ok();
    }
    //send_sample(); TBD
}

void readRegisterCommand(unsigned char unused1, unsigned char unused2) {
    using namespace ADS129x;
    char *arg1;
    arg1 = serialCommand.next();
    if (arg1 != NULL) {
        int registerNumber = hex_to_long(arg1);
        if (registerNumber >= 0) {
            int result = adcRreg(registerNumber);
            printf("200 Ok (Read Register %#x)\n%#x\n",registerNumber,result);
        } else {
            printf("402 Error: expected hexidecimal digits.\n");
        }
    } else {
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
                printf("200 Ok (Write Register %#x %#x)\n",registerNumber,registerValue);
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

void writeRegisterCommandDirect(unsigned char register_number, unsigned char register_value) {
    if (register_number >= 0 && register_value >= 0) {
        adcWreg(register_number, register_value);
        send_response_ok();
    } else {
        send_response_error();
    }
}

void base64ModeOnCommand(unsigned char unused1, unsigned char unused2) {
    base64_mode = true;
    send_response(RESPONSE_OK, "Base64 mode on - rdata command will respond with base64 encoded data.");
}

void hexModeOnCommand(unsigned char unused1, unsigned char unused2) {
    base64_mode = false;
    send_response(RESPONSE_OK, "Hex mode on - rdata command will respond with hex encoded data");
}

void helpCommand(unsigned char unused1, unsigned char unused2) {
    if (protocol_mode == JSONLINES_MODE ||  protocol_mode == MESSAGEPACK_MODE) {
        send_response(RESPONSE_OK, "Help not available in JSON Lines or MessagePack modes.");
    } else {
        printf("200 Ok\n");
        printf("Available commands: \n");
        serialCommand.printCommands();
        printf("\n");
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO); //todo change by command
    //esp_log_level_set("*", ESP_LOG_NONE); //todo change by command
    uart_init();
    spi_init();
    
    using namespace ADS129x;
    adcSendCommand(SDATAC);

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
    serialCommand.addCommand("help", helpCommand);                   // Print list of commands
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
    jsonCommand.addCommand("help", helpCommand);                   // Print list of commands
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

        vTaskDelay(10 / portTICK_RATE_MS); //wait --> see whether this is OK
    }
}
