/*
  Demostración web del clasificador MNIST en la ESP32-CAM
  ==================================================================

  La ESP32-CAM crea su propia red WiFi y sirve una página web con:
    - video en vivo de la cámara con el cuadro que se analiza,
    - botón "Capturar y clasificar" y modo automático,
    - la imagen 28x28 que entra al modelo y las 10 probabilidades,
    - una pestaña para DIBUJAR un dígito (también se clasifica en el chip).

  Todo el aprendizaje automático ocurre aquí: el navegador solo muestra lo que
  el chip calcula. Cada resultado también se imprime en el Monitor Serie.

  Archivos del sketch:
    demo_web.ino   este archivo: WiFi, servidor web, cámara y modelo
    pagina_web.h          la página (HTML + CSS + JavaScript)
    preprocesamiento.h    el MISMO de la Etapa 6 (sin cambios)
    model_data.h          el modelo int8 generado por el notebook (sin cambios)
    camera_pins.h         pines de la ESP32-CAM AI-Thinker

  Uso:
    1. Sube el sketch y abre el Monitor Serie a 115200 baudios.
    2. Conecta el portátil o el celular a la red WiFi "TinyML-MNIST" (clave: tinyml2026).
    3. Abre en el navegador:  http://192.168.4.1

  Comandos del Monitor Serie (funcionan aunque falle el WiFi):
    c = clasificar con la cámara   d = volcar la última imagen 28x28   i = información
*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "img_converters.h"   // frame2jpg(): convierte la imagen en escala de grises a JPEG para el video
#include "camera_pins.h"
#include "model_data.h"
#include "preprocesamiento.h"
#include "pagina_web.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ---------------------------- Configuración ----------------------------
// true: la ESP32-CAM crea su propia red (recomendado para la clase: no depende del WiFi del salón).
// false: se conecta a una red existente (pon su nombre y clave abajo; la IP sale en el Monitor Serie).
constexpr bool MODO_PUNTO_ACCESO = true;
const char* AP_NOMBRE  = "TinyML-MNIST";
const char* AP_CLAVE   = "tinyml2026";     // mínimo 8 caracteres
const char* RED_NOMBRE = "NOMBRE_DE_TU_RED";
const char* RED_CLAVE  = "CLAVE_DE_TU_RED";

// La arena se reserva al arrancar desde el heap de SRAM interna: con WiFi activo, 64 KB fijos
// ya no caben en la región de RAM ESTÁTICA (el enlazador lo detecta). Ajusta con el valor de "arena usada".
constexpr size_t TAM_ARENA = 48 * 1024;
constexpr float UMBRAL_CONFIANZA = 0.60f;
constexpr int CALIDAD_JPEG = 60;           // calidad del video (1-100): más alta = más lenta

// ---------------------------- Estado global ----------------------------
static uint8_t* arena = nullptr;
static tflite::MicroMutableOpResolver<5> resolver;
static tflite::MicroInterpreter* interprete = nullptr;
static TfLiteTensor* entrada = nullptr;
static TfLiteTensor* salida = nullptr;
static int8_t tabla_cuant[256];
static uint8_t img_trabajo[TRAB * TRAB];

static bool estilo_mnist = true, espejo = false, volteo = false, flash = false;

// Dos tareas usan los mismos recursos a la vez (el servidor web y el loop del Monitor Serie;
// además, el video y la clasificación comparten la cámara). Los mutex garantizan turnos:
static SemaphoreHandle_t mutex_camara;   // quién usa la cámara
static SemaphoreHandle_t mutex_modelo;   // quién usa el intérprete y los búferes de preprocesamiento

struct Resultado {
  const char* origen;          // "camara" o "dibujo"
  bool hay_digito;
  int digito;
  float prob[10];
  uint8_t fondo, tinta;
  uint8_t img[MNIST * MNIST];  // la entrada exacta del modelo (para mostrarla)
  uint32_t t_captura_us, t_preproc_us, t_inferencia_us;
};
static Resultado ultimo = {};   // último resultado (para el comando 'd' del Monitor Serie)

static httpd_handle_t servidor_web = nullptr;
static httpd_handle_t servidor_video = nullptr;

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
  config.xclk_freq_hz = 10000000;            // 10 MHz: más margen para escala de grises + WiFi (evita "DMA overflow")
  config.pixel_format = PIXFORMAT_GRAYSCALE; // el modelo necesita grises; el video se convierte a JPEG por software
  config.frame_size = FRAMESIZE_QVGA;        // 320x240, igual que en la Etapa 6
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 2;                       // dos búferes: mientras uno se usa, el otro se llena
  config.grab_mode = CAMERA_GRAB_LATEST;     // siempre entregar la imagen más reciente

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

// ---------------------------- Modelo ----------------------------
bool iniciarModelo() {
  // SRAM interna (rápida), alineada a 16 bytes como exige TFLM.
  arena = (uint8_t*)heap_caps_aligned_alloc(16, TAM_ARENA, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!arena) {
    Serial.println("No hay SRAM interna suficiente para la arena");
    return false;
  }
  const tflite::Model* modelo = tflite::GetModel(g_modelo_mnist);
  if (modelo->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Versión de esquema del modelo incompatible");
    return false;
  }
  if (resolver.AddConv2D() != kTfLiteOk || resolver.AddMaxPool2D() != kTfLiteOk ||
      resolver.AddReshape() != kTfLiteOk || resolver.AddFullyConnected() != kTfLiteOk ||
      resolver.AddSoftmax() != kTfLiteOk) {
    Serial.println("FALLÓ el registro de operaciones");
    return false;
  }
  static tflite::MicroInterpreter interp(modelo, resolver, arena, TAM_ARENA);
  interprete = &interp;
  if (interprete->AllocateTensors() != kTfLiteOk) {
    Serial.println("FALLÓ AllocateTensors");
    return false;
  }
  entrada = interprete->input(0);
  salida = interprete->output(0);
  if (entrada->type != kTfLiteInt8 || entrada->dims->data[1] != MNIST || entrada->dims->data[2] != MNIST ||
      salida->type != kTfLiteInt8 || salida->dims->data[1] != 10) {
    Serial.println("El modelo no tiene la entrada/salida esperada");
    return false;
  }
  const float esc = entrada->params.scale;
  const int zp = entrada->params.zero_point;
  for (int v = 0; v < 256; v++) {
    int q = (int)lroundf(v / 255.0f / esc) + zp;
    tabla_cuant[v] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
  }
  Serial.printf("Modelo OK: %u bytes en flash | arena usada %u de %u bytes\n",
                g_modelo_mnist_len, (unsigned)interprete->arena_used_bytes(), (unsigned)TAM_ARENA);
  return true;
}

// Cuantiza r.img, ejecuta el modelo y llena las probabilidades. Llamar con mutex_modelo tomado.
void inferir(Resultado& r) {
  for (int i = 0; i < MNIST * MNIST; i++) entrada->data.int8[i] = tabla_cuant[r.img[i]];
  uint32_t t0 = micros();
  interprete->Invoke();
  r.t_inferencia_us = micros() - t0;
  r.digito = 0;
  for (int k = 0; k < 10; k++) {
    r.prob[k] = salida->params.scale * (salida->data.int8[k] - salida->params.zero_point);
    if (r.prob[k] > r.prob[r.digito]) r.digito = k;
  }
}

// Captura -> preprocesamiento (Etapa 5) -> inferencia (Etapa 6).
void clasificarCamara(Resultado& r) {
  r = {};
  r.origen = "camara";
  xSemaphoreTake(mutex_modelo, portMAX_DELAY);

  xSemaphoreTake(mutex_camara, portMAX_DELAY);
  uint32_t t0 = micros();
  camera_fb_t* fb = esp_camera_fb_get();
  uint32_t t1 = micros();
  if (fb) {
    ResultadoPreproc p = preprocesar(fb->buf, img_trabajo, r.img, estilo_mnist);
    esp_camera_fb_return(fb);
    r.hay_digito = p.hay_digito;
    r.fondo = p.fondo;
    r.tinta = p.tinta;
  }
  xSemaphoreGive(mutex_camara);
  r.t_captura_us = t1 - t0;
  r.t_preproc_us = micros() - t1;

  if (r.hay_digito) inferir(r);
  ultimo = r;
  xSemaphoreGive(mutex_modelo);
}

// Dibujo del navegador: llega ya como imagen de trabajo 56x56 (trazo blanco sobre negro),
// así que se aplica el mismo paso 5-6 del preprocesamiento que a la cámara.
void clasificarDibujo(const uint8_t* trab56, Resultado& r) {
  r = {};
  r.origen = "dibujo";
  xSemaphoreTake(mutex_modelo, portMAX_DELAY);
  uint32_t t0 = micros();
  memcpy(img_trabajo, trab56, TRAB * TRAB);
  if (estilo_mnist) {
    ResultadoPreproc p = {};
    r.hay_digito = normalizarEstiloMnist(img_trabajo, r.img, p);
  } else {
    reducirSimple(img_trabajo, r.img);
    r.hay_digito = true;
  }
  r.t_preproc_us = micros() - t0;
  if (r.hay_digito) inferir(r);
  ultimo = r;
  xSemaphoreGive(mutex_modelo);
}

void imprimirResultado(const Resultado& r) {
  if (!r.hay_digito) {
    Serial.printf("[%s] No veo ningún dígito (fondo %u, tinta %u)\n", r.origen, r.fondo, r.tinta);
    return;
  }
  bool seguro = r.prob[r.digito] >= UMBRAL_CONFIANZA;
  Serial.printf("[%s] ==> %s %d (%.0f %%) | captura %.1f ms, preproc %.1f ms, inferencia %.1f ms\n",
                r.origen, seguro ? "Dígito:" : "No estoy seguro, quizá", r.digito, 100 * r.prob[r.digito],
                r.t_captura_us / 1000.0f, r.t_preproc_us / 1000.0f, r.t_inferencia_us / 1000.0f);
}

// ---------------------------- Respuestas JSON ----------------------------
void resultadoAJson(const Resultado& r, String& s) {
  s.reserve(4500);
  s = "{\"origen\":\""; s += r.origen; s += "\",\"hay_digito\":"; s += r.hay_digito ? "true" : "false";
  s += ",\"fondo\":"; s += r.fondo; s += ",\"tinta\":"; s += r.tinta;
  s += ",\"t_captura_ms\":"; s += String(r.t_captura_us / 1000.0f, 1);
  s += ",\"t_preproc_ms\":"; s += String(r.t_preproc_us / 1000.0f, 1);
  if (r.hay_digito) {
    s += ",\"t_inferencia_ms\":"; s += String(r.t_inferencia_us / 1000.0f, 1);
    s += ",\"digito\":"; s += r.digito;
    s += ",\"seguro\":"; s += (r.prob[r.digito] >= UMBRAL_CONFIANZA) ? "true" : "false";
    s += ",\"prob\":[";
    for (int k = 0; k < 10; k++) { if (k) s += ','; s += String(r.prob[k], 4); }
    s += "],\"img\":[";
    for (int i = 0; i < MNIST * MNIST; i++) { if (i) s += ','; s += r.img[i]; }
    s += ']';
  }
  s += '}';
}

void estadoAJson(String& s) {
  s = "{\"normalizacion\":"; s += estilo_mnist ? "true" : "false";
  s += ",\"espejo\":"; s += espejo ? "true" : "false";
  s += ",\"volteo\":"; s += volteo ? "true" : "false";
  s += ",\"flash\":"; s += flash ? "true" : "false";
  s += ",\"modelo_bytes\":"; s += g_modelo_mnist_len;
  s += ",\"arena_usada\":"; s += (unsigned)interprete->arena_used_bytes();
  s += ",\"arena_total\":"; s += (unsigned)TAM_ARENA;
  s += ",\"heap_libre\":"; s += (unsigned)ESP.getFreeHeap();
  s += '}';
}

esp_err_t enviarJson(httpd_req_t* req, const String& s) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, s.c_str(), s.length());
}

// ---------------------------- Servidor web (puerto 80) ----------------------------
esp_err_t manejadorPagina(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, PAGINA_HTML, strlen(PAGINA_HTML));
}

esp_err_t manejadorClasificar(httpd_req_t* req) {
  static Resultado r;   // 'static': no ocupa la pila de la tarea del servidor
  clasificarCamara(r);
  imprimirResultado(r);
  String s;
  resultadoAJson(r, s);
  return enviarJson(req, s);
}

esp_err_t manejadorDibujo(httpd_req_t* req) {
  static uint8_t trab56[TRAB * TRAB];
  static Resultado r;
  if (req->content_len != sizeof(trab56)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Se esperaban 3136 bytes (56x56)");
    return ESP_FAIL;
  }
  size_t recibidos = 0;
  while (recibidos < sizeof(trab56)) {
    int n = httpd_req_recv(req, (char*)trab56 + recibidos, sizeof(trab56) - recibidos);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    recibidos += n;
  }
  clasificarDibujo(trab56, r);
  imprimirResultado(r);
  String s;
  resultadoAJson(r, s);
  return enviarJson(req, s);
}

esp_err_t manejadorConfig(httpd_req_t* req) {
  char consulta[64], var[16], val[4];
  if (httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) == ESP_OK &&
      httpd_query_key_value(consulta, "var", var, sizeof(var)) == ESP_OK &&
      httpd_query_key_value(consulta, "val", val, sizeof(val)) == ESP_OK) {
    bool v = atoi(val) != 0;
    if (!strcmp(var, "normalizacion")) estilo_mnist = v;
    else if (!strcmp(var, "espejo")) { espejo = v; aplicarOrientacion(); }
    else if (!strcmp(var, "volteo")) { volteo = v; aplicarOrientacion(); }
    else if (!strcmp(var, "flash")) { flash = v; digitalWrite(FLASH_LED_GPIO_NUM, v ? HIGH : LOW); }
    Serial.printf("Configuración: %s = %d\n", var, v);
  }
  String s;
  estadoAJson(s);
  return enviarJson(req, s);
}

esp_err_t manejadorEstado(httpd_req_t* req) {
  String s;
  estadoAJson(s);
  return enviarJson(req, s);
}

// ---------------------------- Servidor de video (puerto 81) ----------------------------
// MJPEG: una respuesta HTTP que nunca termina y envía un JPEG tras otro.
// Va en otro servidor porque esta función no retorna mientras el navegador mire el video.
#define LIMITE_PARTE "tinymlframe"
esp_err_t manejadorVideo(httpd_req_t* req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" LIMITE_PARTE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char cabecera[80];
  while (true) {
    uint8_t* jpg = nullptr;
    size_t jpg_len = 0;
    xSemaphoreTake(mutex_camara, portMAX_DELAY);
    camera_fb_t* fb = esp_camera_fb_get();
    bool ok = fb && frame2jpg(fb, CALIDAD_JPEG, &jpg, &jpg_len);
    if (fb) esp_camera_fb_return(fb);
    xSemaphoreGive(mutex_camara);
    if (!ok) { free(jpg); vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int n = snprintf(cabecera, sizeof(cabecera),
                     "\r\n--" LIMITE_PARTE "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)jpg_len);
    esp_err_t res = httpd_resp_send_chunk(req, cabecera, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpg, jpg_len);
    free(jpg);
    if (res != ESP_OK) break;        // el navegador cerró el video
    vTaskDelay(pdMS_TO_TICKS(20));   // deja tiempo de CPU a la clasificación
  }
  return ESP_OK;
}

bool iniciarServidores() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.stack_size = 10240;   // el preprocesamiento y la inferencia corren en esta tarea
  config.max_uri_handlers = 8;
  if (httpd_start(&servidor_web, &config) != ESP_OK) return false;
  httpd_uri_t rutas[] = {
    {"/",           HTTP_GET,  manejadorPagina,     nullptr},
    {"/clasificar", HTTP_GET,  manejadorClasificar, nullptr},
    {"/dibujo",     HTTP_POST, manejadorDibujo,     nullptr},
    {"/config",     HTTP_GET,  manejadorConfig,     nullptr},
    {"/estado",     HTTP_GET,  manejadorEstado,     nullptr},
  };
  for (auto& r : rutas) httpd_register_uri_handler(servidor_web, &r);

  httpd_config_t config_video = HTTPD_DEFAULT_CONFIG();
  config_video.server_port = 81;
  config_video.ctrl_port = 32769;
  config_video.stack_size = 8192;
  if (httpd_start(&servidor_video, &config_video) != ESP_OK) return false;
  httpd_uri_t ruta_video = {"/stream", HTTP_GET, manejadorVideo, nullptr};
  httpd_register_uri_handler(servidor_video, &ruta_video);
  return true;
}

// ---------------------------- WiFi ----------------------------
String iniciarWifi() {
  if (MODO_PUNTO_ACCESO) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_NOMBRE, AP_CLAVE);
    WiFi.setSleep(false);
    Serial.printf("Red WiFi creada: \"%s\"  (clave: %s)\n", AP_NOMBRE, AP_CLAVE);
    return WiFi.softAPIP().toString();
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(RED_NOMBRE, RED_CLAVE);
  WiFi.setSleep(false);
  Serial.printf("Conectando a \"%s\"", RED_NOMBRE);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) return "";
  return WiFi.localIP().toString();
}

// ---------------------------- Monitor Serie ----------------------------
void mostrarImg28(const uint8_t* img) {
  const char* rampa = " .:-=+*#%@";
  for (int y = 0; y < MNIST; y++) {
    for (int x = 0; x < MNIST; x++) { char c = rampa[img[y * MNIST + x] * 9 / 255]; Serial.write(c); Serial.write(c); }
    Serial.println();
  }
}

// ---------------------------- Programa principal ----------------------------
static bool listo = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Etapa 9: demostración web del clasificador MNIST ===");
  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_LED_GPIO_NUM, LOW);
  mutex_camara = xSemaphoreCreateMutex();
  mutex_modelo = xSemaphoreCreateMutex();

  if (!iniciarModelo() || !iniciarCamara()) {
    Serial.println("Hay un problema: revisa los mensajes de arriba.");
    return;
  }
  aplicarOrientacion();
  Serial.println("Cámara OK");

  String ip = iniciarWifi();
  if (ip.length() == 0 || !iniciarServidores()) {
    Serial.println("WiFi o servidor web no disponibles. El Monitor Serie sigue funcionando (c, d, i).");
  } else {
    Serial.printf("\n>>> Abre en el navegador:  http://%s\n", ip.c_str());
  }
  Serial.printf("SRAM libre: %u KB | PSRAM libre: %u KB\n",
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  Serial.println("Comandos: c = clasificar, d = volcar 28x28, i = información");
  listo = true;
}

void loop() {
  if (!listo) { delay(1000); return; }
  if (!Serial.available()) { delay(10); return; }
  char cmd = Serial.read();
  if (cmd == 'c') {
    static Resultado r;
    clasificarCamara(r);
    if (r.hay_digito) mostrarImg28(r.img);
    imprimirResultado(r);
  } else if (cmd == 'd') {
    if (!ultimo.hay_digito) { Serial.println("Primero clasifica una imagen"); return; }
    Serial.print("IMG28=");
    for (int i = 0; i < MNIST * MNIST; i++) { Serial.print(ultimo.img[i]); Serial.write(i < MNIST * MNIST - 1 ? ',' : '\n'); }
  } else if (cmd == 'i') {
    Serial.printf("SRAM libre %u KB | PSRAM libre %u KB | arena %u de %u bytes | clientes WiFi: %d\n",
                  (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024),
                  (unsigned)interprete->arena_used_bytes(), (unsigned)TAM_ARENA, WiFi.softAPgetStationNum());
  }
}
