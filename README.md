
# HackEEG Arduino Driver port to ESP32 IDF

The idea was to use the rather cheaper TI ADS1299 EVM module (https://www.ti.com/tool/ADS1299EEGFE-PDK) and use this as a full fledged 8 channel biosignal (e.g. EEG, EDA) amplifier. 
The hardware description will follow ...
The software is based on Arduino code (https://github.com/conorrussomanno/ADS1299 and https://github.com/starcat-io/hackeeg-driver-arduino) which was written for the MEGA2560 or the DUE (using SPI DMA). However, I only had an ESP32 DEVkit 4 so adapted the hackeeg code (https://starcat.io) to this platform. The DEVkit 4 has a CP2102N on board which supposedly allows for a faster USB transfer (>1Mb) as compared to the the CP2102 (max 921600; see https://www.esp32.com/viewtopic.php?t=420). 
The initial attempt to use the Arduino as a module was a nightmare, it finally compiled but never worked...
So finally, I adapted the whole code to the ESP IDF. The most time consuming part was the JSON stuff, but finally I came to like the cJSON library, which is part of the ESP IDF. The SPI register read and write works already, but I have not tested data transfer yet. I was a bit confused about the DMA idea, because the ADS1299 with 8 channels @24bits only transmits (8+1)*3 bytes per data package (i.e. 27 bytes). The limit of the ESP32 is 64 bytes w/o DMA so I implemented it w/o DMA, let's see how this works.    

The idea was to keep the ESP32 code to send/receive identical messages as compared to the Arduino code to be able to use the existing Python interface (https://github.com/starcat-io/hackeeg-client-python)
