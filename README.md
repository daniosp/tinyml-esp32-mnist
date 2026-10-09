# TinyML en ESP32-CAM — reconocimiento de dígitos escritos a mano (MNIST)

Proyecto del curso *Introducción al Aprendizaje Automático* (Maestría).
Una red neuronal convolucional pequeña se entrena con MNIST, se cuantiza a int8 y
se ejecuta **completamente en una ESP32-CAM**: la placa captura una imagen, la preprocesa
y clasifica el dígito en el propio dispositivo. Ninguna imagen sale del microcontrolador.

```
[entrenamiento/ — Google Colab]                               [firmware/ — ESP32-CAM, Arduino]
MNIST → entrenar → aumentar → int8 → .tflite → .h  ──►  cámara → 28×28 → TFLite Micro → dígito
```

## Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `entrenamiento/mnist_tinyml_entrenamiento.ipynb` | Notebook de entrenamiento — **ejecútalo en Google Colab** |
| `entrenamiento/requirements.txt` | Versiones de librerías con las que se probó el notebook |
| `modelos/` | Modelos `.tflite` exportados |
| `firmware/etapa0_prueba_camara/` | Etapa 0: prueba del hardware (cámara, PSRAM, TFLM) |
| `resultados/` | Mediciones: exactitud, latencia, memoria |
| `.github/workflows/build.yml` | CI: compila todo el firmware en cada push |

## Hardware

- **ESP32-CAM-CH340** (ESP32-CAM compatible con AI-Thinker, con USB integrado mediante chip CH340)
  - ESP32 de doble núcleo a 240 MHz, ~520 KB de SRAM interna, 4 MB de PSRAM, 4 MB de flash
  - Cámara OV2640
- Cable micro-USB **de datos** (los cables que solo cargan son una trampa frecuente)

## Versiones de software (fijadas)

| Componente | Versión | Dónde se fija |
|---|---|---|
| Core ESP32 para Arduino | 3.3.12 | `firmware/*/sketch.yaml` |
| TensorFlow Lite Micro | incluido en el core (`esp-tflite-micro`) | a través de la versión del core |
| Driver de cámara | incluido en el core (`esp32-camera`) | a través de la versión del core |
| TensorFlow (entrenamiento) | 2.21.0 | `entrenamiento/requirements.txt` |

---

## 1. Entrenamiento (Google Colab)

Abre `entrenamiento/mnist_tinyml_entrenamiento.ipynb` en Colab (insignia dentro del notebook) y ejecuta todas las celdas.
No se necesita GPU. Al final descarga los archivos generados `mnist_int8.tflite` / `model_data.h`.

## 2. Configuración del firmware en Windows

### 2.1 Instalar el Arduino IDE y el core ESP32
1. Instala **Arduino IDE 2.x** desde <https://www.arduino.cc/en/software>.
2. En `Archivo → Preferencias → Gestor de URLs adicionales de tarjetas`, agrega:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. En `Herramientas → Placa → Gestor de tarjetas`, busca **esp32 by Espressif Systems**,
   elige la versión **3.3.12** en el desplegable e instálala.
   *(Usa la versión fijada, no simplemente "la última", para coincidir con este repositorio.)*

No se necesita ninguna librería adicional: TensorFlow Lite Micro y el driver de la cámara vienen dentro de este core.

### 2.2 Driver USB (CH340)
1. Conecta la placa y abre **Administrador de dispositivos → Puertos (COM y LPT)**.
2. Deberías ver **USB-SERIAL CH340 (COMx)**. Anota el número de COM.
3. Si aparece como dispositivo desconocido, instala el driver CH341SER del fabricante del chip (WCH):
   <https://www.wch-ic.com/downloads/CH341SER_EXE.html>, y vuelve a conectar la placa.
4. Si no aparece nada, prueba con otro cable: probablemente solo sirve para cargar.

### 2.3 Cargar el sketch de la Etapa 0
1. Abre `firmware/etapa0_prueba_camara/etapa0_prueba_camara.ino`.
2. `Herramientas → Placa → esp32 → AI Thinker ESP32-CAM`
3. `Herramientas → Puerto → COMx` (el puerto del CH340).
4. Haz clic en **Subir**.
5. Abre el **Monitor Serie** a **115200 baudios**, presiona el botón **RST** de la placa,
   luego escribe `c` y presiona Enter para capturar una imagen.

Salida esperada:
```
=== Etapa 0: prueba de la ESP32-CAM ===
...
PSRAM           : ENCONTRADA, ~4000 KB libres de ~4000 KB
TFLite Micro    : enlazado OK, 5 operaciones registradas
Cámara OK. PID del sensor: 0x26 (0x26 = OV2640)
```

### 2.4 Compilación reproducible por línea de comandos (opcional, igual que la CI)
```powershell
winget install ArduinoSA.CLI        # o descarga arduino-cli desde https://arduino.github.io/arduino-cli/
cd firmware/etapa0_prueba_camara
arduino-cli compile                       # usa el perfil fijado en sketch.yaml
arduino-cli upload -p COM5                # reemplaza COM5 por tu puerto
arduino-cli monitor -p COM5 -c baudrate=115200
```

### Solución de problemas

| Síntoma | Causa probable → solución |
|---|---|
| `Failed to connect ... No serial data received` / `Wrong boot mode` | La placa no entró en modo de descarga. Mantén presionado **IO0/BOOT**, pulsa **RST**, inicia la carga y suelta IO0 cuando aparezca "Connecting...". Presiona RST al terminar la carga. |
| `Brownout detector was triggered` / reinicios aleatorios | Corriente insuficiente. Usa un cable corto y de buena calidad, conectado directamente a un puerto USB del PC (sin hub). |
| `FALLÓ la inicialización de la cámara: error 0x105` / `0x20001` | El cable plano de la cámara no está bien insertado, o el mapa de pines no es el de AI-Thinker. Vuelve a insertar el conector (con la placa desconectada). |
| `PSRAM : NO ENCONTRADA` | Placa equivocada seleccionada. Elige **AI Thinker ESP32-CAM**. |
| El Monitor Serie muestra caracteres extraños | La velocidad no es 115200 baudios. |
| El Monitor Serie no muestra nada | Presiona RST en la placa. |

## Etapas del proyecto

- [x] Etapa 2 — modelos base (secciones 0–2 del notebook)
- [ ] Etapa 0 — prueba del hardware ← *actual*
- [ ] Etapa 3 — robustez con aumento de datos
- [ ] Etapa 4 — cuantización int8 y exportación
- [ ] Etapas 5–6 — preprocesamiento e inferencia en el dispositivo
- [ ] Etapas 7–8 — verificación y evaluación
- [ ] Etapa 9 — demostración en clase

## Licencia

Por definir (se recomienda MIT para proyectos de curso).
