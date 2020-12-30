/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
//#include "cJSON.h"

#include "SerialCommand.h"
#include "JsonCommand.h"
#include "uart.h"

#define TEXT_MODE 0
#define JSONLINES_MODE 1
#define MESSAGEPACK_MODE 2


const char *STATUS_TEXT_OK = "Ok";
const char *STATUS_TEXT_BAD_REQUEST = "Bad request";
const char *STATUS_TEXT_UNRECOGNIZED_COMMAND = "Unrecognized command";
const char *STATUS_TEXT_ERROR = "Error";
const char *STATUS_TEXT_NOT_IMPLEMENTED = "Not Implemented";
const char *STATUS_TEXT_NO_ACTIVE_CHANNELS = "No Active Channels";

/*const char *COMMAND_KEY = "COMMAND";
const char *PARAMETERS_KEY = "PARAMETERS";
const char *STATUS_CODE_KEY = "STATUS_CODE";
const char *STATUS_TEXT_KEY = "STATUS_TEXT";
const char *HEADERS_KEY = "HEADERS";
const char *DATA_KEY = "DATA"; 
*/

//int protocol_mode = TEXT_MODE;
int protocol_mode = JSONLINES_MODE;

SerialCommand serialCommand; // The  SerialCommand object
JsonCommand jsonCommand;

extern "C"
{
    void app_main();
}

void send_response(int status_code, const char *status_text)
{
    switch (protocol_mode)
    {
    case TEXT_MODE:
        //char response[128];
        //sprintf(response, "%d %s", status_code, status_text);
        //Serial.println(response);
        printf("%d %s\n", status_code, status_text);
        break;
    /*case JSONLINES_MODE:
        jsonCommand.sendJsonLinesResponse(status_code, (char *)status_text);
        break;
    case MESSAGEPACK_MODE:
        // all responses are in JSON Lines, MessagePack mode is only for sending samples
        jsonCommand.sendJsonLinesResponse(status_code, (char *)status_text);
        break;*/
    default:
        // unknown protocol
        ;
    }
}

void send_response_ok()
{
    send_response(RESPONSE_OK, STATUS_TEXT_OK);
}

// This gets set as the default handler, and gets called when no other command matches.
void unrecognized(const char *command)
{
    //Serial.println("406 Error: Unrecognized command.");
    //Serial.println();
    printf("406 Error: Unrecognized command.\n");
    printf("%s\n", command);
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
    printf("Main Free Heap before root %d \n",esp_get_free_heap_size());
    root = cJSON_CreateObject();
    printf("Main Free Heap after root %d \n",esp_get_free_heap_size());
    cJSON_AddItemToObject(root, STATUS_CODE_KEY, cJSON_CreateNumber(STATUS_OK));
    cJSON_AddItemToObject(root, STATUS_TEXT_KEY, cJSON_CreateString(STATUS_TEXT_OK));
    cJSON_AddItemToObject(root, DATA_KEY, cJSON_CreateNumber(microseconds));
    //printf("%s\n", cJSON_Print(root));
    //cJSON_Delete(root);
    switch (protocol_mode)
    {
    case JSONLINES_MODE:
    case MESSAGEPACK_MODE:
        printf("Main Free Heap before sendJsonLinesDocResponse %d \n",esp_get_free_heap_size());
        jsonCommand.sendJsonLinesDocResponse(root);
        break;
    default:
        // unknown protocol
        ;
    }

    /*
    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root[STATUS_CODE_KEY] = STATUS_OK;
    root[STATUS_TEXT_KEY] = STATUS_TEXT_OK;
    root[DATA_KEY] = microseconds;
    switch (protocol_mode)
    {
    case JSONLINES_MODE:
    case MESSAGEPACK_MODE:
        jsonCommand.sendJsonLinesDocResponse(doc);
        break;
    default:
        // unknown protocol
        ;
    }
    */
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

void app_main(void)
{
    uart_init();
    serialCommand.setDefaultHandler(unrecognized);           //
    serialCommand.addCommand("micros", microsCommand);       // Returns number of microseconds since the program began executing
    serialCommand.addCommand("text", textCommand);           // Sets the communication protocol to text
    serialCommand.addCommand("jsonlines", jsonlinesCommand); // Sets the communication protocol to JSONLines
    serialCommand.clearBuffer();

    jsonCommand.setDefaultHandler(unrecognizedJsonLines);  // Handler for any command that isn't matched
    jsonCommand.addCommand("micros", microsCommand);       // Returns number of microseconds since the program began executing
    jsonCommand.addCommand("text", textCommand);           // Sets the communication protocol to text
    jsonCommand.addCommand("jsonlines", jsonlinesCommand); // Sets the communication protocol to JSONLines

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
