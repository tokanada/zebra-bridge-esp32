#pragma once

// Compile-time defaults. Every value can be overridden at runtime through the
// USB console (persisted in NVS, namespace "zbridge").

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ESP32-C3: use UART1. UART0 / GPIO20 / GPIO21 belong to the ROM console;
// GPIO18/19 are the native USB pins used by the CDC console. Never wire the
// printer to any of those pins.
#define ZB_UART_NUM 1
#define ZB_PIN_TX 5
#define ZB_PIN_RX 4
#define ZB_PIN_CTS (-1)
#define ZB_PIN_LED 8
#define ZB_LED_ACTIVE_LOW 1
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3: the printer is reached through the native USB OTG host port
// (GPIO19 = D-, GPIO20 = D+). UART settings are unused by the USB backend but
// stay defined so the shared configuration code keeps compiling.
#define ZB_UART_NUM 1
#define ZB_PIN_TX (-1)
#define ZB_PIN_RX (-1)
#define ZB_PIN_CTS (-1)
#define ZB_PIN_LED 48
#define ZB_LED_ACTIVE_LOW 0
#define ZB_LED_RGB 1
#else
// ESP32-WROOM-32: use UART2.
#define ZB_UART_NUM 2
#define ZB_PIN_TX 17
#define ZB_PIN_RX 16
#define ZB_PIN_CTS (-1)
#define ZB_PIN_LED 2
#define ZB_LED_ACTIVE_LOW 0
#endif

#ifndef ZB_LED_RGB
#define ZB_LED_RGB 0
#endif

#define ZB_DEFAULT_NAME "ZD421-Emulation"
#define ZB_DEFAULT_SERIAL "ZD421-EMULATION-0001"
#define ZB_DEFAULT_BAUD 9600
