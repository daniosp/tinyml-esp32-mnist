/*
  preprocesamiento.h — Etapa 5: de la imagen de la cámara a una imagen 28x28 "estilo MNIST"
  ===========================================================================================

  El modelo aprendió con MNIST, así que cada imagen de la cámara debe llegarle con
  el MISMO formato que esas imágenes. Reproducimos aquí el proceso con el que se
  creó MNIST (LeCun et al.):

    1. Recortar el cuadrado central de la imagen (región de interés, ROI).
    2. Reducir la resolución PROMEDIANDO bloques (evita el aliasing y quita ruido).
    3. Invertir: tinta negra sobre papel blanco -> trazo blanco sobre fondo negro.
    4. Quitar el fondo y estirar el contraste: papel -> 0, tinta más oscura -> 255.
    5. Encontrar la caja que contiene el dígito y escalarla a 20x20 conservando
       la proporción (así no importa a qué distancia esté la cámara).
    6. Colocarla en un lienzo 28x28 con su CENTRO DE MASA en el centro.

  Este archivo es C++ puro (no usa nada de Arduino): se puede compilar y probar
  en un PC con imágenes de ejemplo.
*/
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

constexpr int CAM_ANCHO = 320;               // QVGA en escala de grises: 1 byte por píxel
constexpr int CAM_ALTO  = 240;
constexpr int ROI_LADO  = 224;               // cuadrado central que se analiza
constexpr int FACTOR    = 4;                 // 224 / 4 = 56
constexpr int TRAB      = ROI_LADO / FACTOR; // imagen de trabajo: 56x56
constexpr int MNIST     = 28;                // tamaño de entrada del modelo
constexpr int CAJA      = 20;                // en MNIST el dígito cabe en una caja de 20x20

constexpr int   CONTRASTE_MIN = 40;    // si tinta - fondo < esto, no hay dígito (o hay muy poca luz)
constexpr float ZONA_MUERTA   = 0.25f; // fracción del rango que se considera "todavía papel"
constexpr int   UMBRAL_CAJA   = 64;    // valor (0..255) a partir del cual un píxel cuenta como tinta

struct ResultadoPreproc {
  bool hay_digito;
  uint8_t fondo, tinta;   // niveles estimados tras invertir (diagnóstico)
  int x0, y0, x1, y1;     // caja del dígito en la imagen de trabajo 56x56
};

// Pasos 1-3: recortar el centro, reducir 4x4 -> 1 promediando, e invertir.
static void recortarReducirInvertir(const uint8_t* cam, uint8_t* trab) {
  const int ox = (CAM_ANCHO - ROI_LADO) / 2;
  const int oy = (CAM_ALTO - ROI_LADO) / 2;
  for (int ty = 0; ty < TRAB; ty++) {
    for (int tx = 0; tx < TRAB; tx++) {
      uint32_t suma = 0;
      for (int dy = 0; dy < FACTOR; dy++) {
        const uint8_t* fila = cam + (oy + ty * FACTOR + dy) * CAM_ANCHO + ox + tx * FACTOR;
        for (int dx = 0; dx < FACTOR; dx++) suma += fila[dx];
      }
      trab[ty * TRAB + tx] = 255 - suma / (FACTOR * FACTOR);
    }
  }
}

// Paso 4: el fondo es la MEDIANA (el papel ocupa más de la mitad de la imagen)
// y la tinta es el valor máximo. Todo lo que esté cerca del fondo se vuelve 0.
static bool normalizarContraste(uint8_t* trab, uint8_t& fondo, uint8_t& tinta) {
  const int n = TRAB * TRAB;
  uint16_t hist[256] = {0};
  for (int i = 0; i < n; i++) hist[trab[i]]++;
  int acum = 0, mediana = 0;
  while (acum + hist[mediana] < n / 2) acum += hist[mediana++];
  int maximo = 255;
  while (maximo > 0 && hist[maximo] == 0) maximo--;
  fondo = (uint8_t)mediana;
  tinta = (uint8_t)maximo;
  if (maximo - mediana < CONTRASTE_MIN) return false;

  const float umbral = mediana + ZONA_MUERTA * (maximo - mediana);
  const float ganancia = 255.0f / (maximo - umbral);
  for (int i = 0; i < n; i++) {
    float v = (trab[i] - umbral) * ganancia;
    trab[i] = v <= 0 ? 0 : (v >= 255 ? 255 : (uint8_t)(v + 0.5f));
  }
  return true;
}

// Lectura con interpolación bilineal; fuera de la imagen vale 0 (fondo).
static float bilineal(const uint8_t* img, int lado, float x, float y) {
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  float fx = x - x0, fy = y - y0;
  float v = 0;
  for (int j = 0; j <= 1; j++)
    for (int i = 0; i <= 1; i++) {
      int xi = x0 + i, yj = y0 + j;
      if (xi < 0 || yj < 0 || xi >= lado || yj >= lado) continue;
      float peso = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
      v += peso * img[yj * lado + xi];
    }
  return v;
}

// Pasos 5-6: caja del dígito -> 20x20 (proporción intacta) -> lienzo 28x28 centrado por masa.
static bool normalizarEstiloMnist(const uint8_t* trab, uint8_t* img28, ResultadoPreproc& r) {
  // 5a) Caja que contiene todos los píxeles de tinta.
  r.x0 = TRAB; r.y0 = TRAB; r.x1 = -1; r.y1 = -1;
  for (int y = 0; y < TRAB; y++)
    for (int x = 0; x < TRAB; x++)
      if (trab[y * TRAB + x] >= UMBRAL_CAJA) {
        if (x < r.x0) r.x0 = x;
        if (x > r.x1) r.x1 = x;
        if (y < r.y0) r.y0 = y;
        if (y > r.y1) r.y1 = y;
      }
  if (r.x1 < 0) return false;

  // 5b) Cuadrado del tamaño del lado MAYOR de la caja (así no deformamos el dígito)
  //     y muestreo a 20x20. Promediamos 4x4 sub-muestras por píxel ("supermuestreo"):
  //     al reducir, cada píxel de salida resume toda el área que cubre.
  const int lado = (r.x1 - r.x0 + 1) > (r.y1 - r.y0 + 1) ? (r.x1 - r.x0 + 1) : (r.y1 - r.y0 + 1);
  const float cx = (r.x0 + r.x1 + 1) * 0.5f, cy = (r.y0 + r.y1 + 1) * 0.5f;  // centro de la caja
  const float paso = (float)lado / CAJA;
  const int SS = 4;
  float caja[CAJA * CAJA];
  float masa = 0, mx = 0, my = 0;
  for (int oy = 0; oy < CAJA; oy++)
    for (int ox = 0; ox < CAJA; ox++) {
      float acum = 0;
      for (int sy = 0; sy < SS; sy++)
        for (int sx = 0; sx < SS; sx++) {
          float x = cx - lado * 0.5f + (ox + (sx + 0.5f) / SS) * paso - 0.5f;
          float y = cy - lado * 0.5f + (oy + (sy + 0.5f) / SS) * paso - 0.5f;
          acum += bilineal(trab, TRAB, x, y);
        }
      float v = acum / (SS * SS);
      caja[oy * CAJA + ox] = v;
      masa += v; mx += v * ox; my += v * oy;
    }
  if (masa <= 0) return false;
  mx /= masa; my /= masa;

  // 6) Pegar la caja 20x20 en el lienzo 28x28 de modo que el centro de masa quede en (13.5, 13.5).
  int dx = (int)lroundf(13.5f - mx), dy = (int)lroundf(13.5f - my);
  dx = dx < 0 ? 0 : (dx > MNIST - CAJA ? MNIST - CAJA : dx);   // que no se salga del lienzo
  dy = dy < 0 ? 0 : (dy > MNIST - CAJA ? MNIST - CAJA : dy);

  // Re-estirar para que el trazo más intenso valga 255 (como en MNIST).
  float vmax = 0;
  for (int i = 0; i < CAJA * CAJA; i++) if (caja[i] > vmax) vmax = caja[i];
  memset(img28, 0, MNIST * MNIST);
  for (int y = 0; y < CAJA; y++)
    for (int x = 0; x < CAJA; x++)
      img28[(y + dy) * MNIST + (x + dx)] = (uint8_t)(caja[y * CAJA + x] * 255.0f / vmax + 0.5f);
  return true;
}

// Versión simple (sin caja ni centro de masa): reducir 56x56 -> 28x28 promediando 2x2.
// Sirve para COMPARAR: ¿cuánto aporta la normalización estilo MNIST?
static void reducirSimple(const uint8_t* trab, uint8_t* img28) {
  for (int y = 0; y < MNIST; y++)
    for (int x = 0; x < MNIST; x++) {
      int s = trab[(2 * y) * TRAB + 2 * x] + trab[(2 * y) * TRAB + 2 * x + 1] +
              trab[(2 * y + 1) * TRAB + 2 * x] + trab[(2 * y + 1) * TRAB + 2 * x + 1];
      img28[y * MNIST + x] = (uint8_t)(s / 4);
    }
}

// Tubería completa. cam: imagen QVGA en grises. trab: búfer de 56x56. img28: salida 28x28 (0..255).
static ResultadoPreproc preprocesar(const uint8_t* cam, uint8_t* trab, uint8_t* img28, bool estilo_mnist) {
  ResultadoPreproc r = {};
  recortarReducirInvertir(cam, trab);
  if (!normalizarContraste(trab, r.fondo, r.tinta)) return r;   // hay_digito = false
  if (estilo_mnist) {
    r.hay_digito = normalizarEstiloMnist(trab, img28, r);
  } else {
    reducirSimple(trab, img28);
    r.hay_digito = true;
  }
  return r;
}
