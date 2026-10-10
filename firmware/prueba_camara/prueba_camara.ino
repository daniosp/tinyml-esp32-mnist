/*
  Prueba de funcionamiento del hardware de la ESP32-CAM
  ================================================================

  Objetivo: antes de cualquier aprendizaje automático, comprobar que funciona
  cada pieza de hardware y de software de la que vamos a depender:

    1. La placa arranca y se comunica con el PC por USB serie (CH340).
    2. Se detecta la PSRAM (allí vivirá el búfer de imagen de la cámara).
    3. La cámara captura imágenes en ESCALA DE GRISES (nuestro modelo usa 1 canal).
    4. TensorFlow Lite Micro está disponible y enlaza correctamente (viene
       incluido en el core ESP32 de Arduino: no hay que instalar ninguna
       librería adicional).

  Uso: abre el Monitor Serie a 115200 baudios y envía:
    c  -> capturar una imagen, mostrar estadísticas + vista previa ASCII
    i  -> mostrar información del sistema / memoria
    f  -> encender/apagar el LED flash (muy brillante: no lo mires directamente)

  Placa: "AI Thinker ESP32-CAM"   (FQBN esp32:esp32:esp32cam)
  Core:  esp32 de Espressif 3.3.12 (fijado en sketch.yaml)
*/

#include <Arduino.h>
#include "esp_camera.h"
#include "camera_pins.h"

// "Prueba de humo" de TFLM: si se encuentra este encabezado y el sketch
// enlaza, la librería TensorFlow Lite Micro incluida en el core es utilizable.
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

// El op resolver le indica a TFLM qué operaciones (capas) necesita nuestro
// modelo. Registrar solo estas mantiene el firmware pequeño: los kernels que
// no se usan no se enlazan. Estas cinco son exactamente en las que se
// convierte nuestra CNN pequeña de Keras (Flatten se convierte en Reshape).
static tflite::MicroMutableOpResolver<5> op_resolver;

static bool flash_encendido = false;

// ---------------------------------------------------------------------------
bool iniciarCamara() {
  camera_config_t config = {};
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.xclk_freq_hz = 20000000;

  // ESCALA DE GRISES: 1 byte por píxel (0 = negro, 255 = blanco).
  // Nuestro modelo espera un solo canal, así evitamos convertir desde color.
  config.pixel_format = PIXFORMAT_GRAYSCALE;

  // QVGA = 320x240 -> 76.800 bytes por imagen. Suficientes píxeles para
  // recortar un dígito; más adelante reduciremos el recorte a 28x28.
  config.frame_size = FRAMESIZE_QVGA;

  // El búfer de imagen va en la PSRAM (4 MB de RAM externa), dejando libre la
  // SRAM interna (~520 KB) para el tensor arena de TFLM y el resto del programa.
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("FALLÓ la inicialización de la cámara: error 0x%x (%s)\n", err, esp_err_to_name(err));
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Cámara OK. PID del sensor: 0x%x (0x26 = OV2640)\n", s->id.PID);
  return true;
}

// ---------------------------------------------------------------------------
void mostrarInfoSistema() {
  Serial.println("\n--- Información del sistema ---");
  Serial.printf("Chip            : %s rev %u, %u núcleos @ %u MHz\n",
                ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("Flash           : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
  Serial.printf("Sketch          : %u KB usados, %u KB libres para la aplicación\n",
                (unsigned)(ESP.getSketchSize() / 1024),
                (unsigned)(ESP.getFreeSketchSpace() / 1024));
  Serial.printf("SRAM interna    : %u KB libres de %u KB de heap\n",
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getHeapSize() / 1024));
  Serial.printf("PSRAM           : %s, %u KB libres de %u KB\n",
                psramFound() ? "ENCONTRADA" : "NO ENCONTRADA",
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
}

// ---------------------------------------------------------------------------
void capturarYMostrar() {
  // Con fb_count = 1 el driver guarda la ÚLTIMA imagen capturada esperando a
  // que la pidamos, y puede ser vieja. Tomamos y liberamos una imagen primero
  // para que la que analizamos sea reciente. (Un error clásico con cámaras
  // embebidas.)
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  uint32_t t0 = micros();
  fb = esp_camera_fb_get();
  uint32_t t_captura = micros() - t0;
  if (!fb) {
    Serial.println("FALLÓ la captura");
    return;
  }

  // Estadísticas básicas: una forma rápida de saber si está "muy oscura /
  // muy clara / bien".
  uint32_t suma = 0;
  uint8_t vmin = 255, vmax = 0;
  for (size_t i = 0; i < fb->len; i++) {
    uint8_t v = fb->buf[i];
    suma += v;
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
  }
  Serial.printf("\nImagen %ux%u, %u bytes, captura %u ms | media %u, mín %u, máx %u\n",
                (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len,
                (unsigned)(t_captura / 1000), (unsigned)(suma / fb->len), vmin, vmax);

  // Vista previa ASCII: promediamos bloques de 4x8 píxeles -> 80x30 caracteres.
  // (Los caracteres son el doble de altos que de anchos; por eso 4 por 8.)
  // Píxeles oscuros -> caracteres densos, así la tinta negra sobre papel
  // blanco se ve como '@#'.
  const char *rampa = "@%#*+=-:. ";   // oscuro ... claro
  const int BX = 4, BY = 8;
  for (int y = 0; y + BY <= fb->height; y += BY) {
    for (int x = 0; x + BX <= fb->width; x += BX) {
      uint32_t acum = 0;
      for (int dy = 0; dy < BY; dy++)
        for (int dx = 0; dx < BX; dx++)
          acum += fb->buf[(y + dy) * fb->width + (x + dx)];
      uint8_t prom = acum / (BX * BY);
      Serial.write(rampa[prom * 9 / 255]);
    }
    Serial.println();
  }

  esp_camera_fb_return(fb);  // SIEMPRE devolver el búfer al driver
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Etapa 0: prueba de la ESP32-CAM ===");

  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_LED_GPIO_NUM, LOW);

  mostrarInfoSistema();

  bool tflm_ok = op_resolver.AddConv2D() == kTfLiteOk &&
                 op_resolver.AddMaxPool2D() == kTfLiteOk &&
                 op_resolver.AddReshape() == kTfLiteOk &&
                 op_resolver.AddFullyConnected() == kTfLiteOk &&
                 op_resolver.AddSoftmax() == kTfLiteOk;
  Serial.printf("TFLite Micro    : %s\n",
                tflm_ok ? "enlazado OK, 5 operaciones registradas" : "FALLÓ el registro de operaciones");

  if (!iniciarCamara()) {
    Serial.println("Corrige la cámara antes de continuar (ver la tabla de problemas en el README).");
  }
  Serial.println("\nComandos: c = capturar, i = información, f = LED flash");
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'c': capturarYMostrar(); break;
      case 'i': mostrarInfoSistema(); break;
      case 'f':
        flash_encendido = !flash_encendido;
        digitalWrite(FLASH_LED_GPIO_NUM, flash_encendido ? HIGH : LOW);
        Serial.printf("LED flash %s\n", flash_encendido ? "ENCENDIDO" : "APAGADO");
        break;
      default: break;  // ignorar caracteres de nueva línea / retorno de carro
    }
  }
}
