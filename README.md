
# HackEEG Arduino Driver port to ESP32 IDF

The idea was to use the rather cheaper TI ADS1299 EVM module (https://www.ti.com/tool/ADS1299EEGFE-PDK) and use this as a full fledged 8 channel biosignal (e.g. EEG, EDA) amplifier. 
The hardware description will follow ...
The software is based on Arduino code (https://github.com/conorrussomanno/ADS1299 and https://github.com/starcat-io/hackeeg-driver-arduino) which was written for the MEGA2560 or the DUE (using SPI DMA). However, I only had an ESP32 DEVkit 4 so adapted the hackeeg code (https://starcat.io) to this platform. The DEVkit 4 has a CP2102N on board which supposedly allows for a faster USB transfer (>1Mb) as compared to the the CP2102 (max 921600; see https://www.esp32.com/viewtopic.php?t=420). 

<b>Confirmed:</b> The UART reliably runs at 3M Bauds 
The initial attempt to use the Arduino as a module was a nightmare, it finally compiled but never worked...
So finally, I adapted the whole code to the ESP IDF. The most time consuming part was the JSON stuff, but finally I came to like the cJSON library, which is part of the ESP IDF. The SPI register read and write works already, but I have not tested data transfer yet.
<b>Update:</b> SPI now runs @ 20 MHz and transfer worlks nicely, however letting the ESP handle the CS line was a problem, so I decided to permamntly pull CS L (ADS 1299 exolicitly allows this and it is the only device on the bus)
I was a bit confused about the DMA idea, because the ADS1299 with 8 channels @24bits only transmits (8+1)*3 bytes per data package (i.e. 27 bytes). The limit of the ESP32 is 64 bytes w/o DMA so I implemented it w/o DMA, let's see how this works.    
<b>Update:</b> Polling is much faster and it does the job. I have now implemented a seperate task that waits on a semaphore, which is given by the DRDY ISR. The task then reads the SPI and sends the final buffer over the UART. To save more time one could try to start SPI read in the ISR (using spi_device_polling_start) and then wait before sending the buffer using spi_device_polling_end(spi, portMAX_DELAY). I guess this could give another 10 us or so.

<b>ToDo:</b> 1) all the printf statements need to become uart_write
             2) check in the ISR whether we are still processing the last sample and if necessar skip this one
             3) get rid of all the log messages for heap managment
             4) look at the rdata code (currently does not fdo anything)

The idea was to keep the ESP32 code to send/receive identical messages as compared to the Arduino code to be able to use the existing Python interface (https://github.com/starcat-io/hackeeg-client-python)

The Python code (driver.py) now works. However, it seems to have a speed problem and seems to slow to keep up with SPS > 1000. It is a bit unclear why so amny code/modules are needed to just read 35 bytes ... 