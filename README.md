# TinyML en ESP32-CAM — reconocimiento de dígitos escritos a mano (MNIST)

Proyecto del curso *Introducción al Aprendizaje Automático* (Maestría).
Una red neuronal convolucional pequeña se entrena con MNIST, se cuantiza a int8 y
se ejecuta **completamente en una ESP32-CAM**: la placa captura una imagen, la preprocesa
y clasifica el dígito en el propio dispositivo. Ninguna imagen sale del microcontrolador
para ser clasificada: el navegador (en la demo web) solo muestra lo que el chip calcula.

```
[entrenamiento/ — Google Colab]                               [firmware/ — ESP32-CAM, Arduino]
MNIST → entrenar → aumentar → int8 → .tflite → .h  ──►  cámara → 28×28 → TFLite Micro → dígito
```

## Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `entrenamiento/mnist_tinyml_entrenamiento.ipynb` | Notebook de entrenamiento — **ejecútalo en Google Colab** |
| `entrenamiento/requirements.txt` | Versiones de librerías con las que se probó el notebook |
| `modelos/` | Modelos `.tflite` exportados (`mnist_int8.tflite`) |
| `firmware/prueba_camara/` | Prueba del hardware: cámara, PSRAM y enlace de TFLite Micro |
| `firmware/clasificador/` | Clasificador de dígitos controlado por **Monitor Serie** (sin interfaz web) |
| `firmware/demo_web/` | **Demo principal**: la placa crea una red WiFi y sirve una página web con video en vivo y clasificación |
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

> `model_data.h` es el modelo convertido a un arreglo de C. Cada sketch de `firmware/` tiene su propia
> copia; si reentrenas el modelo, reemplaza `model_data.h` en `firmware/clasificador/` y en `firmware/demo_web/`.

## 2. Configuración del entorno en Windows

### 2.1 Instalar el Arduino IDE y el core ESP32
1. Instala **Arduino IDE 2.x** desde <https://www.arduino.cc/en/software>.
2. En `Archivo → Preferencias → Gestor de URLs adicionales de tarjetas`, agrega:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. En `Herramientas → Placa → Gestor de tarjetas`, busca **esp32 by Espressif Systems**,
   elige la versión **3.3.12** en el desplegable e instálala.
   *(Usa la versión fijada, no simplemente "la última", para coincidir con este repositorio.)*

No se necesita ninguna librería adicional: TensorFlow Lite Micro, el driver de la cámara, el WiFi y el servidor HTTP vienen dentro de este core.

### 2.2 Driver USB (CH340)
1. Conecta la placa y abre **Administrador de dispositivos → Puertos (COM y LPT)**.
2. Deberías ver **USB-SERIAL CH340 (COMx)**. Anota el número de COM.
3. Si aparece como dispositivo desconocido, instala el driver CH341SER del fabricante del chip (WCH):
   <https://www.wch-ic.com/downloads/CH341SER_EXE.html>, y vuelve a conectar la placa.
4. Si no aparece nada, prueba con otro cable: probablemente solo sirve para cargar.

### 2.3 Cómo cargar cualquier sketch
Los tres sketches de `firmware/` se cargan igual:

1. Abre el archivo `.ino` de la carpeta del sketch (por ejemplo `firmware/demo_web/demo_web.ino`).
   Las demás pestañas (`.h`) se abren solas.
2. `Herramientas → Placa → esp32 → AI Thinker ESP32-CAM`
3. `Herramientas → Puerto → COMx` (el puerto del CH340).
4. Haz clic en **Subir**.
5. Abre el **Monitor Serie** a **115200 baudios** y presiona el botón **RST** de la placa.

Si la carga falla con `Wrong boot mode`, mira la tabla de [solución de problemas](#solución-de-problemas).

---

## 3. Firmware

Se recomienda probarlos en este orden:

| Orden | Sketch | Para qué sirve |
|---|---|---|
| 1 | `firmware/prueba_camara/` | Comprobar que la placa, la cámara, la PSRAM y TFLite Micro funcionan |
| 2 | `firmware/clasificador/` | Probar el modelo con dígitos reales usando solo el Monitor Serie |
| 3 | `firmware/demo_web/` | Demo interactiva con página web (la que se usa en clase) |

### 3.1 `prueba_camara` — prueba del hardware

Carga el sketch (sección 2.3), abre el Monitor Serie a 115200 baudios, presiona **RST**,
luego escribe `c` y presiona Enter para capturar una imagen.

Salida esperada (fragmento):
```
PSRAM           : ENCONTRADA, ~4000 KB libres de ~4000 KB
TFLite Micro    : enlazado OK, 5 operaciones registradas
Cámara OK. PID del sensor: 0x26 (0x26 = OV2640)
```

### 3.2 `clasificador` — clasificación por Monitor Serie (sin interfaz web)

Es la versión más simple del sistema completo. No usa WiFi: todo se controla escribiendo
comandos en el Monitor Serie.

```
cámara (320×240, grises) → recorte cuadrado → 28×28 estilo MNIST → cuantizar a int8
                         → TensorFlow Lite Micro → 10 probabilidades → dígito
```

**Archivos del sketch**

| Archivo | Contenido |
|---|---|
| `clasificador.ino` | Cámara, modelo y comandos del Monitor Serie |
| `preprocesamiento.h` | De la imagen de la cámara a la imagen 28×28 estilo MNIST |
| `model_data.h` | Modelo int8 **generado por el notebook** (no editar a mano) |
| `camera_pins.h` | Pines de la ESP32-CAM AI-Thinker |

**Cómo ejecutarlo**

1. Carga el sketch (sección 2.3) y abre el Monitor Serie a **115200 baudios**.
2. Presiona **RST**. Al arrancar, la placa inicializa el modelo (6 pasos numerados) y después la cámara.
   Debe terminar mostrando `Modelo OK`, `Cámara OK` y la lista de comandos.
3. Escribe `p` + Enter para ver una **vista previa** del cuadrado que analiza la cámara y apuntarla.
4. Escribe `c` + Enter para **capturar y clasificar**. Verás la imagen 28×28 que entra al modelo,
   las 10 probabilidades y la línea final con el dígito y los tiempos.
5. Escribe `a` + Enter para el **modo automático** (clasifica cada segundo; útil para una demostración).

**Comandos del Monitor Serie**

| Comando | Acción |
|---|---|
| `p` | Vista previa para apuntar la cámara (solo el cuadrado que se analiza) |
| `c` | Capturar y clasificar, mostrando lo que "ve" el modelo |
| `a` | Modo automático encendido/apagado (clasifica cada segundo) |
| `n` | Normalización estilo MNIST encendida/apagada (para comparar el efecto) |
| `d` | Volcar la última imagen 28×28 como 784 números (para verificarla en Python) |
| `h` / `u` | Espejo horizontal / volteo vertical de la imagen |
| `f` | LED flash encendido/apagado |
| `i` | Información: memoria libre, uso de la arena de TFLM y últimos tiempos |

**Salida de una clasificación** (los valores son un ejemplo y varían):
```
==> Dígito: 7  (98 %)   | captura 35.2 ms, preproc 12.4 ms, inferencia 48.9 ms
```
Si la confianza es menor al 60 % responde `No estoy seguro (lo más probable: …)`, y si no hay
contraste suficiente responde `No veo ningún dígito`.

**Consejos para buenos resultados**
- Escribe el dígito con **plumón negro grueso** sobre **papel blanco**.
- Haz que el dígito llene buena parte del cuadrado de la vista previa (`p`).
- Evita sombras y bordes del papel dentro del cuadro; usa buena luz o el flash (`f`).
- Si el dígito sale reflejado o al revés, usa `h` o `u`.

### 3.3 `demo_web` — demo con servidor web (sketch principal)

Este es el sketch principal para probar el proyecto. La ESP32-CAM **crea su propia red WiFi**
y sirve una página web con:

- Video en vivo de la cámara, con el cuadro que se analiza.
- Botón **Capturar y clasificar** y modo automático.
- La imagen 28×28 que entra al modelo y las 10 probabilidades.
- Una pestaña para **dibujar** un dígito con el dedo o el mouse (también se clasifica en el chip).

Todo el aprendizaje automático ocurre en la placa; cada resultado también se imprime en el Monitor Serie.

**Archivos del sketch**

| Archivo | Contenido |
|---|---|
| `demo_web.ino` | WiFi, servidor web, cámara y modelo |
| `pagina_web.h` | La página (HTML + CSS + JavaScript) incluida como texto en el firmware |
| `preprocesamiento.h` | Mismo preprocesamiento que en `clasificador` |
| `model_data.h` | Modelo int8 generado por el notebook (no editar a mano) |
| `camera_pins.h` | Pines de la ESP32-CAM AI-Thinker |

**Cómo ejecutarlo (modo recomendado: la placa crea su red)**

1. Carga el sketch `firmware/demo_web/demo_web.ino` (sección 2.3) y abre el Monitor Serie a **115200 baudios**.
2. Presiona **RST**. Espera a ver en el Monitor Serie:
   ```
   Modelo OK: ...
   Cámara OK
   Red WiFi creada: "TinyML-MNIST"  (clave: tinyml2026)

   >>> Abre en el navegador:  http://192.168.4.1
   ```
3. Desde el portátil o el celular, conéctate a la red WiFi **`TinyML-MNIST`** con la clave **`tinyml2026`**.
4. Abre en el navegador **`http://192.168.4.1`** (escribe `http://` completo; el navegador no debe cambiarlo a `https`).
5. Apunta la cámara a un dígito, presiona **Capturar y clasificar** (o activa el modo automático)
   y observa el resultado en la página y en el Monitor Serie.

Notas de uso:
- Mientras estás conectado a `TinyML-MNIST` tu equipo **no tendrá internet** (la red es solo de la placa).
  Si el celular ofrece desconectarse por falta de internet, mantén la conexión.
- Esta red no depende del WiFi del salón, por eso es la opción recomendada para la clase.

**Modo alternativo: conectar la placa a una red WiFi existente**

Edita las constantes de configuración al inicio de `demo_web.ino`:

```cpp
constexpr bool MODO_PUNTO_ACCESO = false;     // true = la placa crea su red; false = se conecta a una existente
const char* RED_NOMBRE = "NOMBRE_DE_TU_RED";
const char* RED_CLAVE  = "CLAVE_DE_TU_RED";
```

Sube el sketch otra vez y lee la **IP asignada en el Monitor Serie** (línea `>>> Abre en el navegador: http://…`).
Tu equipo debe estar en la misma red. No subas a GitHub tu nombre de red ni tu clave reales.

**Parámetros que puedes ajustar** (en `demo_web.ino`)

| Constante | Significado |
|---|---|
| `AP_NOMBRE`, `AP_CLAVE` | Nombre y clave de la red que crea la placa (la clave debe tener mínimo 8 caracteres) |
| `TAM_ARENA` | Memoria de trabajo de TFLite Micro (48 KB). Compárala con la "arena usada" que se imprime al arrancar |
| `UMBRAL_CONFIANZA` | Por debajo de este valor (0.60) la respuesta es "no estoy seguro" |
| `CALIDAD_JPEG` | Calidad del video en vivo (1–100); más alta = más lenta |

**Comandos del Monitor Serie** (funcionan aunque falle el WiFi)

| Comando | Acción |
|---|---|
| `c` | Clasificar con la cámara (muestra la imagen 28×28 y el resultado) |
| `d` | Volcar la última imagen 28×28 como 784 números |
| `i` | Información: memoria libre, uso de la arena y clientes WiFi conectados |

**Cómo está organizado el servidor** (para entender o depurar)

La placa ejecuta dos servidores HTTP. El video va aparte porque su respuesta nunca termina
mientras el navegador lo esté mirando.

| Puerto | Ruta | Método | Función |
|---|---|---|---|
| 80 | `/` | GET | La página web |
| 80 | `/clasificar` | GET | Captura, preprocesa, clasifica y responde en JSON |
| 80 | `/dibujo` | POST | Recibe un dibujo de 56×56 píxeles (3136 bytes) y lo clasifica |
| 80 | `/config` | GET | Cambia normalización, espejo, volteo o flash (`?var=…&val=0/1`) |
| 80 | `/estado` | GET | Estado actual y uso de memoria en JSON |
| 81 | `/stream` | GET | Video MJPEG en vivo |

### 3.4 Compilación reproducible por línea de comandos (opcional, igual que la CI)
```powershell
winget install ArduinoSA.CLI        # o descarga arduino-cli desde https://arduino.github.io/arduino-cli/
cd firmware/demo_web                      # o firmware/clasificador, firmware/prueba_camara
arduino-cli compile                       # usa el perfil fijado en sketch.yaml
arduino-cli upload -p COM5                # reemplaza COM5 por tu puerto
arduino-cli monitor -p COM5 -c baudrate=115200
```

---

## Solución de problemas

### Carga y hardware

| Síntoma | Causa probable → solución |
|---|---|
| `Failed to connect ... No serial data received` / `Wrong boot mode` | La placa no entró en modo de descarga. Mantén presionado **IO0/BOOT**, pulsa **RST**, inicia la carga y suelta IO0 cuando aparezca "Connecting...". Presiona RST al terminar la carga. |
| `Brownout detector was triggered` / reinicios aleatorios | Corriente insuficiente. Usa un cable corto y de buena calidad, conectado directamente a un puerto USB del PC (sin hub). Es más frecuente con el WiFi activo en `demo_web`. |
| `FALLÓ la inicialización de la cámara: error 0x105` / `0x20001` | El cable plano de la cámara no está bien insertado, o el mapa de pines no es el de AI-Thinker. Vuelve a insertar el conector (con la placa desconectada). |
| `PSRAM : NO ENCONTRADA` | Placa equivocada seleccionada. Elige **AI Thinker ESP32-CAM**. |
| El Monitor Serie muestra caracteres extraños | La velocidad no es 115200 baudios. |
| El Monitor Serie no muestra nada | Presiona RST en la placa. |

### Clasificación

| Síntoma | Causa probable → solución |
|---|---|
| `No veo ningún dígito (... poco contraste)` | Falta luz o el dígito no está en el cuadro. Usa la vista previa (`p` en `clasificador`), mejora la luz o activa el flash. |
| Predice mal de forma sistemática | Prueba el dígito con trazo más grueso y centrado; compara con la normalización estilo MNIST apagada (`n`); revisa si la imagen está reflejada (`h`) o al revés (`u`). |
| `FALLÓ AllocateTensors` | La arena (`TAM_ARENA`) es pequeña o falta una operación registrada en el resolver. |

### Demo web

| Síntoma | Causa probable → solución |
|---|---|
| No aparece la red `TinyML-MNIST` | Presiona RST y revisa en el Monitor Serie que salió `Red WiFi creada`. Si el Monitor Serie dice `WiFi o servidor web no disponibles`, el servidor no arrancó: usa los comandos `c`, `d`, `i` y revisa los mensajes anteriores. |
| La página no abre | Verifica que estás conectado a `TinyML-MNIST`, usa `http://192.168.4.1` (no `https`) y desactiva temporalmente los datos móviles del celular. |
| En modo red existente no se conecta | Revisa `RED_NOMBRE` y `RED_CLAVE`; la placa solo usa redes de 2.4 GHz. Lee la IP en el Monitor Serie. |
| La página carga pero el video no | El video usa el puerto 81 (`/stream`). Cierra otras pestañas con la demo abierta y recarga. |

