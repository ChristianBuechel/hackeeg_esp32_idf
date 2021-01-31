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
//#ifndef JSONCOMMAND_H
//#define JSONCOMMAND_H
#include <string.h>
#include "stdint.h"

//#include <cJSON.h>
#include "../../json/cJSON/cJSON.h"

// Size of the input buffer in bytes (maximum length of one command plus arguments)
#define JSONCOMMAND_BUFFER 1024
// Maximum length of a command excluding the terminating null
#define JSONCOMMAND_MAXCOMMANDLENGTH 128


#define STATUS_OK 200

#define RESPONSE_OK 200
#define RESPONSE_BAD_REQUEST 400
#define RESPONSE_UNRECOGNIZED_COMMAND 406
#define RESPONSE_WRONG_REG 407
#define RESPONSE_WRONG_REG_VALUE 408
#define RESPONSE_ERROR 500
#define RESPONSE_NOT_IMPLEMENTED 501
#define RESPONSE_NO_ACTIVE_CHANNELS 502


extern const char *COMMAND_KEY;
extern const char *PARAMETERS_KEY;
extern const char *STATUS_CODE_KEY;
extern const char *STATUS_TEXT_KEY;
extern const char *HEADERS_KEY;
extern const char *DATA_KEY;

extern const char *MP_COMMAND_KEY;
extern const char *MP_PARAMETERS_KEY;
extern const char *MP_STATUS_CODE_KEY;
extern const char *MP_STATUS_TEXT_KEY;
extern const char *MP_HEADERS_KEY;
extern const char *MP_DATA_KEY;

extern const char *STATUS_TEXT_OK;
extern const char *STATUS_TEXT_BAD_REQUEST;
extern const char *STATUS_TEXT_UNRECOGNIZED_COMMAND;
extern const char *STATUS_TEXT_WRONG_REG;
extern const char *STATUS_TEXT_WRONG_REG_VAL;
extern const char *STATUS_TEXT_ERROR;
extern const char *STATUS_TEXT_NOT_IMPLEMENTED;
extern const char *STATUS_TEXT_NO_ACTIVE_CHANNELS;


typedef void (*command_func)(unsigned char, unsigned char);

class JsonCommand {
public:
    JsonCommand();      // Constructor
    void addCommand(const char *command, void (*func)(unsigned char register_number,
                                                      unsigned char register_value)); // Add a command to the processing dictionary.
    void setDefaultHandler(void (*function)(const char *));   // A handler to call when no valid command received.

    void readSerial();                           // Main entry point.
    void readSerialMessagePackMessage();         // Entry point for MessagePack mode
    void clearBuffer();     // Clears the input buffer.
    void printCommands();   // Prints the list of commands.
    char * next();           // Returns pointer to next token found in command buffer (for getting arguments to commands).
    void sendJsonLinesResponse(int status_code, char *status_text);    // send a simple JSON Lines response
    void sendJsonLinesDocResponse(cJSON *doc);                  // send a JsonDocument as a JSON Lines response
    //void sendMessagePackResponse(int status_code, char *status_text);  // send a simple MessagePack response
    //void sendMessagePackDocResponse(JsonDocument &doc);                // send a JsonDocument as a MessagePack response

private:
    // Command/handler dictionary
    struct JsonCommandCallback {
        char command[JSONCOMMAND_MAXCOMMANDLENGTH + 1];
        command_func command_function;
    };                                    // Data structure to hold Command/Handler function key-value pairs
    JsonCommandCallback *commandList;     // Actual definition for command/handler array
    uint8_t commandCount;

    // Pointer to the default handler function
    void (*defaultHandler)(const char *);

    char delim[2]; // null-terminated list of character to be used as delimeters for tokenizing (default " ")
    char term;     // Character that signals end of command (default '\n')

    char buffer[JSONCOMMAND_BUFFER + 1]; // Buffer of stored characters while waiting for terminator character
    uint8_t bufPos;                      // Current position in the buffer
    char *last;                          // State variable used by strtok_r during processing

    int findCommand(const char *command);
};

//#endif  // JSONCOMMAND_H
