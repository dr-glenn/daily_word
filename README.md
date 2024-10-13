# _Daily Word_

Fetch the word-of-the-day from a website and display it with definition on ePaper display.

## Hardware
ESP32 (any variant)

Waveshare 2.7" or 2.9" ePaper display. Others will probably work with minimal code changes.

## Software
This is an ESP-IDF project that I developed in VSCode. It cannot be built with Arduino IDE AFAIK.
The main program fetches a daily web page from Merriam-Webster. It searches the page for sequences that mark the word, the type, the pronounciation, and the definition and extracts the contents of those 4 sections.

The web content is very long, since it's filled with trackers and much formatting. Therefore the entire page is not downloaded and stored. Instead http_sream_read.cpp and stream_buf.c are used to fetch and scan the page in chunks (currently 2048 bytes). When the start of useful content is found, the characters are then copied to a larger buffer until the end of content is encountered.

I determined that 20000 is adequate for the large buffer (#define STREAM_BUF_LEN 20000 in stream_buf.h).

The ePaper software is currently only able to display characters with no accents, so some words will appear incorrect.

## Components
There are two components: epaper and wifi.

**ePaper** is software from Waveshare. It includes primitive bitmapped font files (only standard ASCII). It writes to the display by bit-banging the SPI interface; I would like to improve that!

**wifi** is directly derived from an ESP-IDF example and simplified a bit. Obviously it conencts to WiFi.

## SDKConfig File
The epaper and wifi components have Kconfig.projbuild. When you run menuconfig, you will see options to choose the epaper display and to specify your WiFi SSID and password. If you get the WiFi credentials wrong the program may run in an endless loop with stack traces.
To prevent endless reboot, use these settings in sdkconfig or change with menuconfig:
```
CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y
#CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT is not set
```

