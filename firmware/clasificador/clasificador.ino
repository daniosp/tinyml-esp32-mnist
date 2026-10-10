/*
  Clasificador de dígitos MNIST en la ESP32-CAM (todo en el dispositivo)
  =====================================================================================

  cámara (320x240) -> preprocesamiento (28x28 estilo MNIST) -> cuantizar a int8
                   -> TensorFlow Lite Micro -> 10 probabilidades -> dígito

  Archivos (pestañas) de este sketch:
    etapa6_clasificador.ino  este archivo: cámara, modelo, comandos
    preprocesamiento.h       Etapa 5: de la imagen de la cámara a 28x28 estilo MNIST
    model_data.h             el modelo int8, GENERADO por el notebook (Etapa 4): no editar
    camera_pins.h            pines de la ESP32-CAM AI-Thinker (copia de la Etapa 0)

  Monitor Serie a 115200 baudios. Comandos:
    p  vista previa para APUNTAR la cámara (solo el cuadrado que se analiza)
    c  capturar y clasificar, mostrando lo que "ve" el modelo
    a  modo automático encendido/apagado (clasifica cada segundo; ideal para la demo)
    n  normalización estilo MNIST encendida/apagada (para comparar)
    d  volcar la última imagen 28x28 como números (para verificarla en Python, Etapa 7)
    h  espejo horizontal   u  voltear vertical   f  LED flash   i  información

  Consejos: escribe el dígito con PLUMÓN negro grueso sobre papel blanco, llena
  buena parte del cuadrado y evita sombras y bordes del papel dentro del cuadro.
*/

#include <Arduino.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "model_data.h"
#include "preprocesamiento.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ---------------------------- Configuración ----------------------------
// Tensor arena: la RAM de trabajo de TFLM (entrada, salida y activaciones intermedias).
// Empezamos con margen; el comando 'i' muestra cuánto se usa realmente para ajustarlo.
constexpr size_t TAM_ARENA = 64 * 1024;
constexpr float UMBRAL_CONFIANZA = 0.60f;   // por debajo, respondemos "no estoy seguro"
constexpr uint32_t PERIODO_AUTO_MS = 1000;

// La arena va en la SRAM interna (más rápida que la PSRAM). alignas(16): TFLM lo requiere.
alignas(16) static uint8_t arena[TAM_ARENA];

static tflite::MicroMutableOpResolver<5> resolver;
static tflite::MicroInterpreter* interprete = nullptr;
static TfLiteTensor* entrada = nullptr;
static TfLiteTensor* salida = nullptr;
static int8_t tabla_cuant[256];   // píxel 0..255 -> int8, según escala/punto cero del modelo

static uint8_t img_trabajo[TRAB * TRAB];
static uint8_t img28[MNIST * MNIST];   // última entrada del modelo (0..255, estilo MNIST)
static bool hay_img28 = false;
static bool estilo_mnist = true, modo_auto = false, flash = false, espejo = false, volteo = false;
static uint32_t ultimo_auto = 0;
static uint32_t t_captura_us = 0, t_preproc_us = 0, t_inferencia_us = 0;

// ---------------------------- Cámara ----------------------------
bool iniciarCamara() {
  camera_config_t config = {};
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;   config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;   config.pin_reset = RESET_GPIO_NUM;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;   // 1 canal, como MNIST
  config.frame_size = FRAMESIZE_QVGA;          // 320x240: debe coincidir con CAM_ANCHO/CAM_ALTO
  config.fb_location = CAMERA_FB_IN_PSRAM;     // 75 KB de imagen en la PSRAM
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("FALLÓ la inicialización de la cámara: error 0x%x (%s)\n", err, esp_err_to_name(err));
    return false;
  }
  return true;
}

void aplicarOrientacion() {
  sensor_t* s = esp_camera_sensor_get();
  s->set_hmirror(s, espejo ? 1 : 0);
  s->set_vflip(s, volteo ? 1 : 0);
}

// Devuelve una imagen RECIENTE (descarta la que el driver tenía guardada).
camera_fb_t* capturar() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (fb && (fb->width != CAM_ANCHO || fb->height != CAM_ALTO || fb->format != PIXFORMAT_GRAYSCALE)) {
    Serial.println("La imagen no es QVGA en escala de grises: revisa iniciarCamara()");
    esp_camera_fb_return(fb);
    return nullptr;
  }
  return fb;
}

// ---------------------------- Modelo ----------------------------
// PASO(): imprime un punto de control y ESPERA a que salga por el puerto serie (flush).
// Si el chip se reinicia, el último PASO impreso dice exactamente dónde ocurrió.
#define PASO(texto) do { Serial.printf("  [modelo] %s\n", texto); Serial.flush(); } while (0)

bool iniciarModelo() {
  PASO("1/6 leyendo el modelo de la flash (GetModel)");
  const tflite::Model* modelo = tflite::GetModel(g_modelo_mnist);
  if (modelo->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("Versión de esquema del modelo %lu != %d esperada\n",
                  (unsigned long)modelo->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }
  Serial.printf("  [modelo] dirección del modelo: %p (alineada a 16: %s), %u bytes\n",
                g_modelo_mnist, ((uintptr_t)g_modelo_mnist % 16 == 0) ? "sí" : "NO", g_modelo_mnist_len);

  PASO("2/6 registrando las 5 operaciones");
  if (resolver.AddConv2D() != kTfLiteOk || resolver.AddMaxPool2D() != kTfLiteOk ||
      resolver.AddReshape() != kTfLiteOk || resolver.AddFullyConnected() != kTfLiteOk ||
      resolver.AddSoftmax() != kTfLiteOk) {
    Serial.println("FALLÓ el registro de operaciones");
    return false;
  }

  PASO("3/6 creando el intérprete");
  static tflite::MicroInterpreter interp(modelo, resolver, arena, TAM_ARENA);
  interprete = &interp;

  PASO("4/6 AllocateTensors (prepara cada capa y reparte la arena)");
  if (interprete->AllocateTensors() != kTfLiteOk) {
    Serial.println("FALLÓ AllocateTensors: arena muy pequeña o falta una operación en el resolver");
    return false;
  }

  PASO("5/6 verificando entrada y salida");
  entrada = interprete->input(0);
  salida = interprete->output(0);
  if (entrada->type != kTfLiteInt8 || entrada->dims->size != 4 || entrada->dims->data[1] != MNIST ||
      entrada->dims->data[2] != MNIST || salida->type != kTfLiteInt8 || salida->dims->data[1] != 10) {
    Serial.println("El modelo no tiene la entrada/salida esperada (int8 28x28 -> int8 10)");
    return false;
  }

  // Tabla de cuantización: q = round(x / escala) + punto_cero, con x = píxel / 255.
  const float esc = entrada->params.scale;
  const int zp = entrada->params.zero_point;
  for (int v = 0; v < 256; v++) {
    int q = (int)lroundf(v / 255.0f / esc) + zp;
    tabla_cuant[v] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
  }

  PASO("6/6 inferencia de prueba con una imagen negra");
  for (int i = 0; i < MNIST * MNIST; i++) entrada->data.int8[i] = tabla_cuant[0];
  if (interprete->Invoke() != kTfLiteOk) { Serial.println("FALLÓ Invoke() de prueba"); return false; }

  Serial.printf("Modelo OK: %u bytes en flash | arena usada %u de %u bytes\n",
                g_modelo_mnist_len, (unsigned)interprete->arena_used_bytes(), (unsigned)TAM_ARENA);
  Serial.printf("Entrada: escala=%.8f punto_cero=%d | Salida: escala=%.8f punto_cero=%d\n",
                esc, zp, salida->params.scale, (int)salida->params.zero_point);
  return true;
}

// ---------------------------- Visualización ----------------------------
// Lo que entra al modelo: trazo blanco sobre negro -> valores altos = caracteres densos.
void mostrarImg28() {
  const char* rampa = " .:-=+*#%@";
  for (int y = 0; y < MNIST; y++) {
    for (int x = 0; x < MNIST; x++) {
      char c = rampa[img28[y * MNIST + x] * 9 / 255];
      Serial.write(c); Serial.write(c);   // 2 caracteres por píxel: compensa que son altos
    }
    Serial.println();
  }
}

// Para apuntar: el cuadrado de 224x224 que se analiza, tal como lo ve la cámara.
void vistaPrevia() {
  camera_fb_t* fb = capturar();
  if (!fb) return;
  const char* rampa = "@%#*+=-:. ";   // oscuro (tinta) ... claro (papel)
  const int ox = (CAM_ANCHO - ROI_LADO) / 2, oy = (CAM_ALTO - ROI_LADO) / 2;
  const int BX = 4, BY = 8;
  Serial.println("\nVista previa del cuadrado analizado (centra el dígito aquí):");
  for (int y = 0; y < ROI_LADO; y += BY) {
    for (int x = 0; x < ROI_LADO; x += BX) {
      uint32_t s = 0;
      for (int dy = 0; dy < BY; dy++)
        for (int dx = 0; dx < BX; dx++) s += fb->buf[(oy + y + dy) * CAM_ANCHO + ox + x + dx];
      Serial.write(rampa[(s / (BX * BY)) * 9 / 255]);
    }
    Serial.println();
  }
  esp_camera_fb_return(fb);
}

// ---------------------------- Clasificación ----------------------------
void clasificar(bool detallado) {
  uint32_t t0 = micros();
  camera_fb_t* fb = capturar();
  if (!fb) { Serial.println("FALLÓ la captura"); return; }
  uint32_t t1 = micros();

  // Etapa 5: preprocesamiento (ver preprocesamiento.h)
  ResultadoPreproc r = preprocesar(fb->buf, img_trabajo, img28, estilo_mnist);
  esp_camera_fb_return(fb);   // ya no necesitamos la imagen: devolver el búfer al driver
  if (!r.hay_digito) {
    Serial.printf("No veo ningún dígito (fondo %u, tinta %u: poco contraste). ¿Hay luz y un dígito centrado?\n",
                  r.fondo, r.tinta);
    hay_img28 = false;
    return;
  }
  hay_img28 = true;
  for (int i = 0; i < MNIST * MNIST; i++) entrada->data.int8[i] = tabla_cuant[img28[i]];
  uint32_t t2 = micros();

  // Etapa 6: inferencia
  if (interprete->Invoke() != kTfLiteOk) { Serial.println("FALLÓ Invoke()"); return; }
  uint32_t t3 = micros();
  t_captura_us = t1 - t0; t_preproc_us = t2 - t1; t_inferencia_us = t3 - t2;

  // Salida int8 -> probabilidades: p = escala * (q - punto_cero)
  float prob[10];
  int mejor = 0;
  for (int k = 0; k < 10; k++) {
    prob[k] = salida->params.scale * (salida->data.int8[k] - salida->params.zero_point);
    if (prob[k] > prob[mejor]) mejor = k;
  }

  if (detallado) {
    Serial.printf("\nLo que ve el modelo (28x28, normalización estilo MNIST %s):\n",
                  estilo_mnist ? "ENCENDIDA" : "APAGADA");
    mostrarImg28();
    Serial.println("Probabilidades:");
    for (int k = 0; k < 10; k++) {
      Serial.printf("  %d  %5.1f %% ", k, 100 * prob[k]);
      for (int j = 0; j < (int)(prob[k] * 40 + 0.5f); j++) Serial.write('#');
      Serial.println(k == mejor ? "  <--" : "");
    }
  }
  if (prob[mejor] >= UMBRAL_CONFIANZA)
    Serial.printf("==> Dígito: %d  (%.0f %%)", mejor, 100 * prob[mejor]);
  else
    Serial.printf("==> No estoy seguro (lo más probable: %d con %.0f %%)", mejor, 100 * prob[mejor]);
  Serial.printf("   | captura %.1f ms, preproc %.1f ms, inferencia %.1f ms\n",
                t_captura_us / 1000.0f, t_preproc_us / 1000.0f, t_inferencia_us / 1000.0f);
}

// Imprime la última entrada como 784 números (0..255) en una línea, para pegarla en Python.
void volcarImg28() {
  if (!hay_img28) { Serial.println("Primero clasifica una imagen con 'c'"); return; }
  Serial.print("IMG28=");
  for (int i = 0; i < MNIST * MNIST; i++) {
    Serial.print(img28[i]);
    Serial.write(i < MNIST * MNIST - 1 ? ',' : '\n');
  }
}

void mostrarInfo() {
  Serial.println("\n--- Información ---");
  Serial.printf("SRAM interna libre: %u KB | PSRAM libre: %u KB\n",
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  if (interprete)
    Serial.printf("Tensor arena: %u bytes usados de %u reservados\n",
                  (unsigned)interprete->arena_used_bytes(), (unsigned)TAM_ARENA);
  Serial.printf("Última medición: captura %.1f ms, preproc %.1f ms, inferencia %.1f ms\n",
                t_captura_us / 1000.0f, t_preproc_us / 1000.0f, t_inferencia_us / 1000.0f);
  Serial.printf("Normalización MNIST %s | espejo %s | volteo %s | modo auto %s\n",
                estilo_mnist ? "sí" : "no", espejo ? "sí" : "no", volteo ? "sí" : "no", modo_auto ? "sí" : "no");
}

// ---------------------------- Programa principal ----------------------------
static bool listo = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Etapas 5-6: clasificador MNIST en la ESP32-CAM ===");
  Serial.printf("SRAM libre %u KB | PSRAM %s (%u KB libres)\n", (unsigned)(ESP.getFreeHeap() / 1024),
                psramFound() ? "encontrada" : "NO ENCONTRADA", (unsigned)(ESP.getFreePsram() / 1024));
  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_LED_GPIO_NUM, LOW);

  // Primero el MODELO y después la CÁMARA: así, si algo falla, sabemos cuál de los dos fue.
  Serial.println("Iniciando modelo...");  Serial.flush();
  bool modelo_ok = iniciarModelo();
  Serial.println("Iniciando cámara...");  Serial.flush();
  bool cam_ok = iniciarCamara();
  if (cam_ok) { aplicarOrientacion(); Serial.println("Cámara OK"); }
  listo = cam_ok && modelo_ok;
  if (!listo) {
    Serial.println("Hay un problema: revisa los mensajes de arriba.");
    return;
  }
  Serial.println("\nComandos: p=apuntar  c=clasificar  a=auto  n=normalización  d=volcar  h/u=orientación  f=flash  i=info");
}

void loop() {
  if (!listo) { delay(1000); return; }

  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'p': vistaPrevia(); break;
      case 'c': clasificar(true); break;
      case 'a': modo_auto = !modo_auto; Serial.printf("Modo automático %s\n", modo_auto ? "ENCENDIDO" : "APAGADO"); break;
      case 'n': estilo_mnist = !estilo_mnist; Serial.printf("Normalización estilo MNIST %s\n", estilo_mnist ? "ENCENDIDA" : "APAGADA"); break;
      case 'd': volcarImg28(); break;
      case 'h': espejo = !espejo; aplicarOrientacion(); Serial.printf("Espejo horizontal %s\n", espejo ? "sí" : "no"); break;
      case 'u': volteo = !volteo; aplicarOrientacion(); Serial.printf("Volteo vertical %s\n", volteo ? "sí" : "no"); break;
      case 'f': flash = !flash; digitalWrite(FLASH_LED_GPIO_NUM, flash ? HIGH : LOW); Serial.printf("LED flash %s\n", flash ? "ENCENDIDO" : "APAGADO"); break;
      case 'i': mostrarInfo(); break;
      default: break;   // ignorar saltos de línea
    }
  }

  if (modo_auto && millis() - ultimo_auto >= PERIODO_AUTO_MS) {
    ultimo_auto = millis();
    clasificar(false);
  }
}
