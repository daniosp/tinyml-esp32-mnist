/*
  pagina_web.h — Página de la demostración (HTML + CSS + JavaScript en un solo texto).
  La sirve la propia ESP32-CAM. El navegador SOLO muestra: la captura, el preprocesamiento
  y la inferencia ocurren en el chip, que responde con los resultados en JSON.
*/
#pragma once

const char PAGINA_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TinyML MNIST</title>
<link rel="icon" href="data:,">
<style>
  :root { --fondo:#f4f4f1; --panel:#ffffff; --texto:#1d1d1b; --suave:#6b6b66; --borde:#deded8;
          --acento:#2a78d6; --ok:#1a7f4b; --duda:#b25e00; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
         background:var(--fondo); color:var(--texto); }
  header { padding:14px 20px; border-bottom:1px solid var(--borde); background:var(--panel);
           display:flex; flex-wrap:wrap; gap:8px 20px; align-items:baseline; }
  header h1 { margin:0; font-size:20px; }
  header span { color:var(--suave); font-size:14px; }
  main { display:grid; grid-template-columns:minmax(0,3fr) minmax(0,2fr); gap:16px; padding:16px;
         max-width:1200px; margin:0 auto; }
  @media (max-width:800px) { main { grid-template-columns:1fr; } }
  .panel { background:var(--panel); border:1px solid var(--borde); border-radius:10px; padding:14px; }
  .pestanas { display:flex; gap:6px; margin-bottom:12px; }
  .pestanas button { flex:1; padding:10px; border:1px solid var(--borde); background:var(--fondo);
                     border-radius:8px; font-size:15px; cursor:pointer; color:var(--texto); }
  .pestanas button.activa { background:var(--acento); color:#fff; border-color:var(--acento); }
  .vista { position:relative; width:100%; aspect-ratio:4/3; background:#111; border-radius:8px; overflow:hidden; }
  .vista img { width:100%; height:100%; display:block; object-fit:fill; }
  /* Cuadro analizado: 224x224 centrado en 320x240 -> 15% a los lados, 3.33% arriba y abajo */
  .roi { position:absolute; left:15%; top:3.333%; width:70%; height:93.333%;
         border:2px dashed #ffd23f; border-radius:4px; pointer-events:none; }
  .roi small { position:absolute; top:6px; left:6px; color:#ffd23f; font-size:13px; font-weight:600;
               background:rgba(0,0,0,.6); padding:2px 6px; border-radius:4px; }
  #lienzo { width:100%; max-width:420px; aspect-ratio:1; display:block; margin:0 auto;
            background:#000; border-radius:8px; touch-action:none; cursor:crosshair; }
  .botones { display:flex; flex-wrap:wrap; gap:8px; margin-top:12px; }
  .boton { padding:12px 18px; font-size:16px; border-radius:8px; border:1px solid var(--borde);
           background:var(--fondo); cursor:pointer; color:var(--texto); }
  .boton.principal { background:var(--acento); color:#fff; border-color:var(--acento); font-weight:600; }
  .opciones { display:flex; flex-wrap:wrap; gap:6px 16px; margin-top:12px; font-size:14px; color:var(--suave); }
  .opciones label { display:flex; align-items:center; gap:6px; cursor:pointer; }
  .resultado { display:flex; align-items:center; gap:16px; }
  #digito { font-size:96px; font-weight:700; line-height:1; min-width:70px; text-align:center; }
  #confianza { font-size:20px; font-weight:600; }
  #mensaje { color:var(--suave); font-size:14px; margin-top:4px; }
  h2 { font-size:14px; text-transform:uppercase; letter-spacing:.05em; color:var(--suave); margin:16px 0 8px; }
  .fila28 { display:flex; gap:14px; align-items:flex-start; }
  #img28 { width:140px; height:140px; image-rendering:pixelated; background:#000; border-radius:4px; flex:none; }
  .nota { font-size:13px; color:var(--suave); }
  .barra { display:grid; grid-template-columns:16px 1fr 52px; gap:8px; align-items:center; font-size:14px;
           margin:3px 0; font-variant-numeric:tabular-nums; }
  .barra .pista { background:var(--fondo); border-radius:4px; height:14px; overflow:hidden; }
  .barra .relleno { background:#b9c9dd; height:100%; width:0; border-radius:4px; transition:width .2s; }
  .barra.mejor .relleno { background:var(--acento); }
  .barra.mejor { font-weight:700; }
  .datos { display:grid; grid-template-columns:auto 1fr; gap:4px 12px; font-size:14px; font-variant-numeric:tabular-nums; }
  .datos dt { color:var(--suave); }
  .datos dd { margin:0; }
  footer { text-align:center; color:var(--suave); font-size:13px; padding:8px 16px 20px; }
</style>
</head>
<body>
<header>
  <h1>TinyML · Reconocimiento de dígitos en la ESP32-CAM</h1>
  <span>La inferencia ocurre en el microcontrolador; esta página solo muestra los resultados.</span>
</header>

<main>
  <section class="panel">
    <div class="pestanas">
      <button id="tabCamara" class="activa">Cámara</button>
      <button id="tabDibujo">Dibujar</button>
    </div>

    <div id="modoCamara">
      <div class="vista">
        <img id="video" alt="Video en vivo de la ESP32-CAM">
        <div class="roi"><small>zona analizada</small></div>
      </div>
      <div class="botones">
        <button class="boton principal" id="btnCapturar">Capturar y clasificar</button>
        <label class="boton"><input type="checkbox" id="chkAuto"> Modo automático</label>
      </div>
    </div>

    <div id="modoDibujo" hidden>
      <canvas id="lienzo" width="280" height="280"></canvas>
      <div class="botones">
        <button class="boton principal" id="btnDibujo">Clasificar dibujo</button>
        <button class="boton" id="btnBorrar">Borrar</button>
      </div>
      <p class="nota">Dibuja un dígito grande con el mouse o el dedo. Se envía a la ESP32-CAM, que aplica el
        mismo preprocesamiento y el mismo modelo que con la cámara.</p>
    </div>

    <div class="opciones">
      <label><input type="checkbox" id="normalizacion" checked> Normalización estilo MNIST</label>
      <label><input type="checkbox" id="espejo"> Espejo</label>
      <label><input type="checkbox" id="volteo"> Voltear</label>
      <label><input type="checkbox" id="flash"> LED flash</label>
    </div>
  </section>

  <section class="panel">
    <div class="resultado">
      <div id="digito">–</div>
      <div>
        <div id="confianza">Sin resultado</div>
        <div id="mensaje">Pulsa «Capturar y clasificar».</div>
      </div>
    </div>

    <h2>Lo que ve el modelo (28×28)</h2>
    <div class="fila28">
      <canvas id="img28" width="28" height="28"></canvas>
      <p class="nota">Esta es la imagen exacta que entra a la red, después del preprocesamiento en el chip:
        trazo blanco sobre fondo negro, como en MNIST.</p>
    </div>

    <h2>Probabilidades</h2>
    <div id="barras"></div>

    <h2>Medido en el dispositivo</h2>
    <dl class="datos">
      <dt>Captura</dt><dd id="tCaptura">–</dd>
      <dt>Preprocesamiento</dt><dd id="tPreproc">–</dd>
      <dt>Inferencia</dt><dd id="tInferencia">–</dd>
      <dt>Modelo (flash)</dt><dd id="dModelo">–</dd>
      <dt>Tensor arena (RAM)</dt><dd id="dArena">–</dd>
    </dl>
  </section>
</main>

<footer>Modelo CNN int8 · TensorFlow Lite Micro · ESP32-CAM</footer>

<script>
const $ = (id) => document.getElementById(id);
const URL_STREAM = location.protocol + "//" + location.hostname + ":81/stream";
let ocupado = false, temporizador = null;

// ---------- Barras de probabilidad ----------
for (let k = 0; k < 10; k++) {
  $("barras").insertAdjacentHTML("beforeend",
    `<div class="barra" id="b${k}"><span>${k}</span><div class="pista"><div class="relleno"></div></div><span>–</span></div>`);
}

// ---------- Video ----------
function iniciarVideo() { $("video").src = URL_STREAM + "?t=" + Date.now(); }
function detenerVideo() { $("video").src = "data:,"; }   // cierra la conexión del stream

// ---------- Mostrar un resultado (JSON enviado por la ESP32-CAM) ----------
function mostrar(r) {
  $("tCaptura").textContent = r.origen === "dibujo" ? "— (dibujo)" : r.t_captura_ms.toFixed(1) + " ms";
  $("tPreproc").textContent = r.t_preproc_ms.toFixed(1) + " ms";
  if (!r.hay_digito) {
    $("digito").textContent = "?";
    $("confianza").textContent = "No veo ningún dígito";
    $("mensaje").textContent = "Poco contraste: revisa la luz y que el dígito esté dentro del cuadro.";
    $("tInferencia").textContent = "–";
    return;
  }
  const p = r.prob[r.digito];
  $("digito").textContent = r.digito;
  $("digito").style.color = r.seguro ? "var(--ok)" : "var(--duda)";
  $("confianza").textContent = (100 * p).toFixed(0) + " % de confianza";
  $("mensaje").textContent = r.seguro ? "Clasificado en el dispositivo." : "Confianza baja: no estoy seguro.";
  $("tInferencia").textContent = r.t_inferencia_ms.toFixed(1) + " ms";
  for (let k = 0; k < 10; k++) {
    const fila = $("b" + k);
    fila.classList.toggle("mejor", k === r.digito);
    fila.querySelector(".relleno").style.width = (100 * r.prob[k]).toFixed(1) + "%";
    fila.lastElementChild.textContent = (100 * r.prob[k]).toFixed(1) + "%";
  }
  const ctx = $("img28").getContext("2d");
  const im = ctx.createImageData(28, 28);
  for (let i = 0; i < 784; i++) {
    const v = r.img[i];
    im.data[4 * i] = im.data[4 * i + 1] = im.data[4 * i + 2] = v;
    im.data[4 * i + 3] = 255;
  }
  ctx.putImageData(im, 0, 0);
}

async function pedir(url, opciones) {
  if (ocupado) return;
  ocupado = true;
  try {
    const resp = await fetch(url, opciones);
    mostrar(await resp.json());
  } catch (e) {
    $("mensaje").textContent = "Error de conexión con la ESP32-CAM.";
  } finally {
    ocupado = false;
  }
}

// ---------- Cámara ----------
$("btnCapturar").onclick = () => pedir("/clasificar");
$("chkAuto").onchange = (e) => {
  clearInterval(temporizador);
  if (e.target.checked) temporizador = setInterval(() => pedir("/clasificar"), 1500);
};

// ---------- Dibujo ----------
const lienzo = $("lienzo"), lctx = lienzo.getContext("2d");
function borrar() { lctx.fillStyle = "#000"; lctx.fillRect(0, 0, 280, 280); }
borrar();
lctx.strokeStyle = "#fff"; lctx.lineWidth = 22; lctx.lineCap = "round"; lctx.lineJoin = "round";
let dibujando = false;
function punto(e) {
  const r = lienzo.getBoundingClientRect();
  return [(e.clientX - r.left) * 280 / r.width, (e.clientY - r.top) * 280 / r.height];
}
lienzo.onpointerdown = (e) => { dibujando = true; lienzo.setPointerCapture(e.pointerId);
  const [x, y] = punto(e); lctx.beginPath(); lctx.moveTo(x, y); lctx.lineTo(x, y); lctx.stroke(); };
lienzo.onpointermove = (e) => { if (!dibujando) return; const [x, y] = punto(e); lctx.lineTo(x, y); lctx.stroke(); };
lienzo.onpointerup = lienzo.onpointercancel = () => { dibujando = false; };
$("btnBorrar").onclick = borrar;
$("btnDibujo").onclick = () => {
  // Reducir 280x280 -> 56x56 (el tamaño de la imagen de trabajo del chip) y enviar 1 byte por píxel.
  const c = document.createElement("canvas"); c.width = c.height = 56;
  const cx = c.getContext("2d"); cx.imageSmoothingEnabled = true; cx.imageSmoothingQuality = "high";
  cx.drawImage(lienzo, 0, 0, 56, 56);
  const d = cx.getImageData(0, 0, 56, 56).data, bytes = new Uint8Array(56 * 56);
  for (let i = 0; i < bytes.length; i++) bytes[i] = d[4 * i];
  pedir("/dibujo", { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: bytes });
};

// ---------- Pestañas ----------
function modo(camara) {
  $("tabCamara").classList.toggle("activa", camara);
  $("tabDibujo").classList.toggle("activa", !camara);
  $("modoCamara").hidden = !camara;
  $("modoDibujo").hidden = camara;
  if (camara) { iniciarVideo(); } else { detenerVideo(); $("chkAuto").checked = false; clearInterval(temporizador); }
}
$("tabCamara").onclick = () => modo(true);
$("tabDibujo").onclick = () => modo(false);

// ---------- Opciones y estado ----------
function mostrarEstado(s) {
  for (const k of ["normalizacion", "espejo", "volteo", "flash"]) $(k).checked = s[k];
  $("dModelo").textContent = (s.modelo_bytes / 1024).toFixed(1) + " KB";
  $("dArena").textContent = (s.arena_usada / 1024).toFixed(1) + " KB usados de " + (s.arena_total / 1024).toFixed(0) + " KB";
}
for (const k of ["normalizacion", "espejo", "volteo", "flash"]) {
  $(k).onchange = async (e) => {
    const r = await fetch(`/config?var=${k}&val=${e.target.checked ? 1 : 0}`);
    mostrarEstado(await r.json());
  };
}
fetch("/estado").then((r) => r.json()).then(mostrarEstado).catch(() => {});
modo(true);
</script>
</body>
</html>
)rawliteral";
