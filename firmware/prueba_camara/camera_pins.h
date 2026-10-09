// camera_pins.h — Conexión de pines GPIO entre el ESP32 y la cámara OV2640.
//
// Placa: ESP32-CAM-CH340 (ESP32-CAM compatible con AI-Thinker, con chip
// USB-serie CH340 integrado). El cableado de la cámara es idéntico al de la
// ESP32-CAM de AI-Thinker, así que usamos su mapa de pines (tomado del
// ejemplo oficial CameraWebServer de arduino-esp32, core 3.3.12).
//
// Si esp_camera_init() falla con estos pines, probablemente tu placa es una
// variante distinta: es lo primero que hay que revisar.

#pragma once

#define PWDN_GPIO_NUM   32   // apagado (power-down) de la cámara
#define RESET_GPIO_NUM  -1   // no conectado (reset por software)
#define XCLK_GPIO_NUM    0   // reloj que el ESP32 le entrega a la cámara
#define SIOD_GPIO_NUM   26   // datos SCCB (similar a I2C) — configuración de la cámara
#define SIOC_GPIO_NUM   27   // reloj SCCB (similar a I2C)

// Bus paralelo de datos de píxel de 8 bits
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5

#define VSYNC_GPIO_NUM  25   // señal de "nueva imagen"
#define HREF_GPIO_NUM   23   // señal de "fila válida"
#define PCLK_GPIO_NUM   22   // reloj de píxel

#define FLASH_LED_GPIO_NUM 4 // el LED blanco muy brillante junto a la cámara
