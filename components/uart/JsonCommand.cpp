/**
 * JsonCommand - A Wiring/Arduino library that uses JsonLines as
 * a protocol for sending commands and receiving data
 * over a serial port.
 *
 * Copyright (C) 2013-2019 Adam Feuer <adam@adamfeuer.com>
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
 */

// uncomment for debugging on Serial interface (programming port)
// you must connect to Serial port first, then SerialUSB, since Serial will reset the Arduino Due
//#define JSONCOMMAND_DEBUG 1

#include "JsonCommand.h"
#include "stdlib.h"
#include "ctype.h"
#include "stdint.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "uart.h"

#define TAG "JsonCommand"

const char *COMMAND_KEY = "COMMAND";
const char *PARAMETERS_KEY = "PARAMETERS";
const char *STATUS_CODE_KEY = "STATUS_CODE";
const char *STATUS_TEXT_KEY = "STATUS_TEXT";
const char *HEADERS_KEY = "HEADERS";
const char *DATA_KEY = "DATA";

const char *MP_COMMAND_KEY = "C";
const char *MP_PARAMETERS_KEY = "P";
const char *MP_STATUS_CODE_KEY = "C";
const char *MP_STATUS_TEXT_KEY = "T";
const char *MP_HEADERS_KEY = "H";
const char *MP_DATA_KEY = "D";
/**
 * Constructor makes sure some things are set.
 */
JsonCommand::JsonCommand()
    : commandList(NULL),
      commandCount(0),
      defaultHandler(NULL),
      term('\n'), // default terminator for commands, newline character
      last(NULL)
{

    strcpy(delim, " "); // strtok_r needs a null-terminated string
    clearBuffer();
}

/**
 * Adds a "command" and a handler function to the list of available commands.
 * This is used for matching a found token in the buffer, and gives the pointer
 * to the handler function to deal with it.
 */
void JsonCommand::addCommand(const char *command,
                             void (*func)(unsigned char register_number, unsigned char register_value))
{

    commandList = (JsonCommandCallback *)realloc(commandList, (commandCount + 1) * sizeof(JsonCommandCallback));
    strncpy(commandList[commandCount].command, command, JSONCOMMAND_MAXCOMMANDLENGTH);
    commandList[commandCount].command_function = func;
    commandCount++;
}

/**
 * This sets up a handler to be called in the event that the receveived command string
 * isn't in the list of commands.
 */
void JsonCommand::setDefaultHandler(void (*func)(const char *))
{
    defaultHandler = func;
}

/**
 * This checks the Serial stream for characters, and assembles them into a buffer.
 * When the terminator character (default '\n') is seen, it starts parsing the
 * buffer as a JSON document (JSONLines).
 *
 * Then it checks to see if it has the proper structure as a command. If so,
 * it runs the command and outputs the result as a JSONLines document to the 
 * Serial stream.
 */
void JsonCommand::readSerial()
{
    int length = 0;
    uart_get_buffered_data_len(UART_NUM_0, (size_t *)&length);

    //StaticJsonDocument<1024> json_command;
    cJSON *cj_root, *cj_array, *cj_temp;
    //while (Serial.available() > 0) {
    while (length > 0)
    {
        uint8_t inChar;
        length = uart_read_bytes(UART_NUM_0, &inChar, 1, 100);
        uart_get_buffered_data_len(UART_NUM_0, (size_t *)&length);
        //char inChar = Serial.read();   // Read single available character, there may be more waiting

        if (inChar == term)
        { // Check for the terminator (default '\r') meaning end of command
            //DeserializationError error = deserializeJson(json_command, buffer);
            //printf("Buffer: %s\n", buffer);
            ESP_LOGI(TAG, "Free Heap before parse %d",esp_get_free_heap_size());
            cj_root = cJSON_Parse(buffer);
            ESP_LOGI(TAG, "Free Heap after parse %d",esp_get_free_heap_size());

            /*printf("cj_root.type %d\n",cj_root->type);
            printf("cj_root.string %s\n",cj_root->string);
            printf("cj_root.valuestring %s\n",cj_root->valuestring);*/

            //if (error)

            if (cj_root == NULL)
            {
                clearBuffer();
                //sendJsonLinesResponse(400, (char *)"Bad Request");
                sendJsonLinesResponse(RESPONSE_BAD_REQUEST, (char *)STATUS_TEXT_BAD_REQUEST);                
                ESP_LOGI(TAG, "Free Heap bad request %d",esp_get_free_heap_size());
                return;
            }

            //printf("vor temp\n");
            cj_temp = cJSON_GetObjectItem(cj_root, COMMAND_KEY); 
            ESP_LOGI(TAG, "Free Heap after cj_temp %d",esp_get_free_heap_size());

            //printf("nach temp\n");
            //char *command = (char *)malloc(100);
            char *command;
            command = cJSON_GetStringValue(cj_temp);
            if (command == NULL)
            {
                clearBuffer();
                sendJsonLinesResponse(RESPONSE_UNRECOGNIZED_COMMAND, (char *)STATUS_TEXT_UNRECOGNIZED_COMMAND);
                cJSON_Delete(cj_root);
                ESP_LOGI(TAG, "Free Heap cmd NULL %d",esp_get_free_heap_size());//OK
                return;
            }
            //printf("command: %s\n", command);
            //JsonObject command_object = json_command.as<JsonObject>();
            //JsonVariant command_name_variant = command_object.getMember(COMMAND_KEY);
            //if (command_name_variant.isNull())
            if (command[0] == '\0')
            {
                (*defaultHandler)("");  
                clearBuffer();
                cJSON_Delete(cj_root);
                ESP_LOGI(TAG, "Free Heap cmd 0 %d",esp_get_free_heap_size());
                return;
            }
            //const char *command = command_name_variant.as<const char *>();
            int command_num = findCommand(command);
            if (command_num < 0)
            {
                (*defaultHandler)(command);
                clearBuffer();
                cJSON_Delete(cj_root);
                ESP_LOGI(TAG, "Free Heap cmd num <0 %d",esp_get_free_heap_size());
                return;
            }

            cj_array = cJSON_GetObjectItem(cj_root, PARAMETERS_KEY);
            ESP_LOGI(TAG, "Free Heap after cj_array %d",esp_get_free_heap_size());

            int array_size = cJSON_GetArraySize(cj_array);
            //printf("array_size: %d\n", array_size);
            //JsonVariant parameters_variant = json_command.getMember(PARAMETERS_KEY);

            unsigned char register_number = 0;
            unsigned char register_value = 0;

            //if (!parameters_variant.isNull())

            if (array_size > 0)
            {
                register_number = cJSON_GetArrayItem(cj_array, 0)->valueint;
                ESP_LOGI(TAG, "register_number: %d", register_number);
                if (array_size > 1)
                {
                    register_value = cJSON_GetArrayItem(cj_array, 1)->valueint;
                    ESP_LOGI(TAG, "register_value: %d", register_value);
                }
                /*
                JsonArray params_array = parameters_variant.as<JsonArray>();
                size_t number_of_params = params_array.size();
                if (number_of_params > 0)
                {
                    register_number = params_array[0];
                }
                if (number_of_params > 1)
                {
                    register_value = params_array[1];
                }*/
            }
            ESP_LOGI(TAG, "Free Heap before del %d",esp_get_free_heap_size());
            //cJSON_Delete(cj_array);
            //printf("Free Heap after del array %d \n",esp_get_free_heap_size());
            //cJSON_Delete(cj_temp);
            //printf("Free Heap after del temp %d \n",esp_get_free_heap_size());
            cJSON_Delete(cj_root);
            ESP_LOGI(TAG, "Free Heap after del root %d",esp_get_free_heap_size());
            // Execute the stored handler function for the command
            (*commandList[command_num].command_function)(register_number, register_value);
            clearBuffer();
        }
        else
        {
            //if (bufPos < JSONCOMMAND_BUFFER) {
            buffer[bufPos++] = inChar; // Put character into buffer
            buffer[bufPos] = '\0';     // Null terminate
            /*} else {
#ifdef JSONCOMMAND_DEBUG
                Serial.println("Line buffer is full - increase JSONCOMMAND_BUFFER");
#endif
            }*/
        }
    }
}

int JsonCommand::findCommand(const char *command)
{
    int result = -1;
    for (int i = 0; i < commandCount; i++)
    {
        if (strcmp(command, commandList[i].command) == 0)
        {
            result = i;
            break;
        }
    }
    return result;
}

void JsonCommand::sendJsonLinesResponse(int status_code, char *status_text)
{
    /*StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root[STATUS_CODE_KEY] = status_code;
    root[STATUS_TEXT_KEY] = status_text;
    serializeJson(doc, Serial);
    Serial.println();
    doc.clear();*/

    cJSON *response;
    ESP_LOGI(TAG, "Bad r before create %d",esp_get_free_heap_size());
    response = cJSON_CreateObject();
    ESP_LOGI(TAG, "Bad r after create %d",esp_get_free_heap_size());
    cJSON_AddItemToObject(response, STATUS_CODE_KEY, cJSON_CreateNumber(status_code));
    cJSON_AddItemToObject(response, STATUS_TEXT_KEY, cJSON_CreateString(status_text));
    ESP_LOGI(TAG, "Bad r after add %d",esp_get_free_heap_size());
    
    char* text = cJSON_Print(response);
    printf("%s\n", text);
    free(text);

    cJSON_Delete(response);
    ESP_LOGI(TAG, "Bad r after delete %d",esp_get_free_heap_size());

}

void JsonCommand::sendJsonLinesDocResponse(cJSON *doc)
{
    /*serializeJson(doc, Serial);
    Serial.println();
    doc.clear();*/
    ESP_LOGI(TAG, "sendJsonLinesDocResponse Free Heap before del %d",esp_get_free_heap_size());
    
    char* text = cJSON_Print(doc);
    printf("%s\n", text);
    //printf("and now directly to UART (%d bytes)\n",strlen(text));
    //uart_write(text, strlen(text));
    free(text);

    cJSON_Delete(doc);
    ESP_LOGI(TAG, "sendJsonLinesDocResponse Free Heap after del %d",esp_get_free_heap_size());
}

/*void JsonCommand::sendMessagePackResponse(int status_code, char *status_text)
{
    StaticJsonDocument<1024> doc;
    JsonObject root = doc.to<JsonObject>();
    root[MP_STATUS_CODE_KEY] = status_code;
    if (!status_text)
    {
        root[MP_STATUS_TEXT_KEY] = status_text;
    }
    serializeMsgPack(doc, Serial);
    doc.clear();
}

void JsonCommand::sendMessagePackDocResponse(JsonDocument &doc)
{
    serializeMsgPack(doc, Serial);
    doc.clear();
}
*/

/**
 * Clear the input buffer.
 */
void JsonCommand::clearBuffer()
{
    buffer[0] = '\0';
    bufPos = 0;
}

/**
 * Retrieve the next token ("word" or "argument") from the command buffer.
 * Returns NULL if no more tokens exist.
 */
char *JsonCommand::next()
{
    return strtok_r(NULL, delim, &last);
}
