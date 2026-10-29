# KylieAI firmware notes (fork de 78/xiaozhi-esp32)

Placa objetivo: **Spotpear ESP32-S3-Touch-LCD-1.28 BOX** (board dir `main/boards/spotpear/sp-esp32-s3-1.28-box`, ESP32-S3 N16R8, GC9A01 240x240 redondo, touch CST816D, ES8311, 16 MB flash).
Redactado el 2026-10-28 contra upstream commit `6b2990b` (firmware v2.5.3). Sin hardware aun: todo lo de "comportamiento en placa" esta deducido del codigo, no probado.

Ramas: `main` = espejo limpio del upstream. `kylie` = nuestros cambios (solo archivos nuevos: este doc, `scripts/kylie-build.sh`, `main/boards/spotpear/sp-esp32-s3-1.28-box/kylie.json`). No se modifico ningun archivo del upstream.

---

## 1. Toolchain y build

| Cosa | Valor |
|---|---|
| ESP-IDF requerido | **>= v6.0.1**, recomendado rama `release/v6.1` (IDF 5.x NO soportado). `build.py` default 6.0.2. No usar el tag `v6.1` (bug Wi-Fi en C3, issue #2344) |
| ESP-IDF instalado | `~/esp/esp-idf` = rama `release/v6.1` (reporta `ESP-IDF v6.1.0`), herramientas en `~/.espressif` (toolchain xtensa-esp-elf 15.2.0) |
| Target | `esp32s3` |
| Idioma | `es-ES` (textos y digitos hablados del codigo de vinculacion en espanol) |
| Wake word local | `wn9_hiesp` ("Hi ESP", AFE, la elige build.py para idiomas no chinos). El toque en pantalla / boton BOOT tambien inicia conversacion |

Compilar (genera `kylie.json` con la URL y llama a `scripts/build.py`):

```sh
cd ~/Documents/kylie-firmware
KYLIE_HOST=192.168.1.50 ./scripts/kylie-build.sh     # IP real del Mac con Docker
./scripts/kylie-build.sh                             # sin KYLIE_HOST => 192.168.PLACEHOLDER
```

Variables: `KYLIE_HOST`, `KYLIE_PORT` (8000), `KYLIE_SCHEME` (http), `KYLIE_LANG` (es-ES), `IDF_PATH`. El primer build completo tarda varios minutos (descarga de componentes gestionados + compilacion); los siguientes son incrementales.

**Resultado del build de hoy: VERDE.** Artefactos (en `build/`, ignorado por git):

- `build/merged-binary.bin` (9.6 MB, flashear en offset 0x0) <- el que se usa el viernes
- `build/xiaozhi.bin` 2.6 MB (app), `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`, `build/ota_data_initial.bin`, `build/generated_assets.bin` (fuentes/emojis, 0x800000)
- Variante: nombre reportado `sp-esp32-s3-1.28-box-kylie`, tipo `sp-esp32-s3-1.28-box`

IMPORTANTE: la URL compilada es `http://192.168.PLACEHOLDER:8000/xiaozhi/ota/`. Con el placeholder el aparato NO conectara. El viernes: o recompilas con `KYLIE_HOST=<ip>` o pones la URL en la pagina de configuracion Wi-Fi (campo "Custom OTA URL", sin recompilar; ver seccion 5).

---

## 2. Como funciona REALMENTE el aparato (esto cambia el plan)

El firmware xiaozhi **no se configura con una URL de WebSocket**. Solo conoce una **URL OTA** (`CONFIG_OTA_URL`, sobreescribible en NVS `wifi/ota_url`). Todo lo demas lo entrega el servidor en la respuesta OTA:

1. Arranca, conecta Wi-Fi (si no hay SSID guardado: modo AP de configuracion).
2. `POST <OTA_URL>` (HTTP, con headers `Device-Id`=MAC, `Client-Id`=UUID, `Activation-Version: 1`, `User-Agent`, `Accept-Language`, body JSON con `mac_address`, `uuid`, `application.version`, `board`, ...). Espera **HTTP 200 + JSON**.
3. La respuesta puede traer: `server_time`, `firmware{version,url}`, `activation{code,message,challenge}`, `websocket{url,token,version}` o `mqtt{...}`. Las claves de `websocket` (strings/numeros) se guardan tal cual en NVS namespace `websocket`.
4. **Si la respuesta NO trae `websocket`, el firmware cae a MQTT** (`InitializeProtocol`). Sin OTA endpoint propio, nunca llegara a nuestro `/ws/device`.
5. Si trae `activation.code`: muestra `activation.message` en pantalla y **dice los digitos en voz alta** (sonidos del idioma), y entra a un bucle de espera (ver 4.C).
6. El WebSocket **solo se abre bajo demanda** (wake word, toque o boton BOOT) y se cierra al terminar. Con el aparato en reposo **no hay conexion** con el servidor.

Autenticacion WS: headers del handshake `Authorization: Bearer <websocket.token>` (si el token no contiene espacio, el firmware antepone "Bearer "), `Protocol-Version: 1`, `Device-Id: <MAC>`, `Client-Id: <uuid>`. **El hello del dispositivo NO lleva `device_key`.**

---

## 3. Diferencias de protocolo: firmware real vs `docs/contracts/WEBSOCKET.md`

Fuente: `main/protocols/websocket_protocol.cc`, `main/protocols/protocol.cc`, `main/application.cc`, `docs/websocket.md`.

### 3.1 Lo que YA es compatible (el gateway actual lo cubre)

- Endpoint/headers: el gateway ya lee `Authorization: Bearer` (`_token_from_request`). OK.
- Hello servidor: ya incluye `"transport":"websocket"` (el firmware lo EXIGE; sin eso ignora el hello y da timeout a los 10 s), `session_id` y `audio_params`.
- `audio_params.sample_rate` del hello servidor: el gateway hace eco del 16000 del dispositivo. **Mantenerlo**: si el hello omite sample_rate, el firmware asume 24000 y decodifica mal.
- Audio: Opus mono 16 kHz, 60 ms, frames binarios crudos (Protocol-Version 1) en ambas direcciones. Coincide con el contrato.
- `listen start|stop`, `stt`, `tts start|stop|sentence_start` (con `text`): mismos nombres y semantica.
- `abort`: coincide (el firmware agrega `session_id` y a veces `reason:"wake_word_detected"`).

### 3.2 Diferencias exactas

| Tema | Contrato Kylie | Firmware real | Accion |
|---|---|---|---|
| Descubrimiento WS | URL fija `ws://host:8000/ws/device` | El firmware obtiene `websocket.url` y `websocket.token` de la **respuesta OTA** (`POST /xiaozhi/ota/`), sin eso usa MQTT | **GATEWAY: crear `POST /xiaozhi/ota/`** (4.A) |
| Auth | `device_key` en hello o Bearer | Solo header `Authorization: Bearer <token>`; token = `websocket.token` de OTA. Hello NO trae `device_key` | Gateway: el token que entrega OTA debe ser el `device_key` (>= 8 chars). No depender de `hello.device_key` |
| Identidad | `device_key` | `Device-Id` (MAC) y `Client-Id` (UUID) en headers/OTA | Usar MAC como identidad al emitir la key en OTA |
| Vinculacion | `{"type":"pairing","code":"482913"}` por WS tras hello | **El mensaje `pairing` no existe.** Cae en `Unknown message type` y se ignora. El codigo se muestra solo si viene en `activation.code` de la respuesta OTA, ANTES de abrir WS | **GATEWAY: devolver el codigo en OTA** (4.A/4.C). El `pairing` por WS puede quedarse (inofensivo) |
| `display` (`emotion` idle/listening/thinking/speaking + text) | Servidor -> dispositivo | **No manejado** (warning "Unknown message type"). El equivalente es `{"type":"llm","emotion":"<nombre>"}` | Gateway: ademas de `display`, enviar `llm`. Mapeo: idle->`neutral`, listening->`neutral`, thinking->`thinking`, speaking->`neutral` (o `happy`). Nombres validos del pack de emojis: neutral happy laughing funny sad angry crying loving embarrassed surprised shocked thinking winking cool relaxed delicious kissy confident sleepy silly confused. El texto de `display` se ignora: el firmware ya muestra "Escuchando"/"Hablando" y los subtitulos via `stt`/`sentence_start` |
| `wake` (proactivo) | Servidor inicia interaccion | **No existe** y, peor, **no hay socket abierto en reposo** (6 en seccion 2). No hay forma de que el servidor "despierte" al aparato | **FIRMWARE: parche** (4.D). Tipo `notify` (docs/notify.md) existe pero tambien necesita canal de control abierto (MQTT en stock) |
| `state` (battery/charging) | Dispositivo -> servidor periodico | **El firmware no lo envia nunca.** Tampoco va bateria en el body OTA | Gateway: tolerar su ausencia (ya lo hace). Opcional firmware: parche para enviarlo |
| Formato PCM en dev | `hello.audio_params.format="pcm"` permitido | El aparato siempre dice `"format":"opus"`; no hay modo PCM | Servidor necesita libopus operativo en el contenedor (`codec.OPUS_AVAILABLE`); PCM solo para el simulador |
| `listen.mode` | `"auto"` | `"auto"` (AEC off, nuestro caso), `"manual"`, `"realtime"` (si AEC activo). Tambien `listen` con `state:"detect"` + `text` (wake word) | Gateway: ya ignora `detect` en debug; tratar `manual` igual que `auto`+stop explicito |
| Campos extra en mensajes del dispositivo | no existen | `session_id` en todo mensaje, `features`, `text_font`, `transport`, `audio_params.channels` en hello | Ignorar (el gateway ya ignora extras) |
| Mensajes extra del firmware | - | `mcp` (JSON-RPC, tools del dispositivo), `system{command:reboot}`, `alert{status,message,emotion}`, `notify{audio_url}` (servidor->dispositivo) | Opcionales; `alert` sirve para avisos con sonido |
| Timeout de canal | - | El firmware marca el canal caido si pasan **120 s sin ningun frame entrante** (PING WS no cuenta; `OnData` si) | Si se hace canal persistente (4.D), el gateway debe enviar un JSON liviano cada ~30-45 s (cualquier `type` valido, p.ej. `llm neutral`) |
| Reconexion | - | Un hello nuevo del mismo device reemplaza al anterior (gateway cierra 4000). El aparato abre/cierra WS en cada interaccion | Esperado; no es bug |
| TLS | `wss://` | `ws://` funciona en LAN; `wss://` usa el bundle de CAs de Mozilla | Para el viernes usar `ws://` y `http://` |

### 3.3 Mensajes del dispositivo que el servidor recibira (referencia)

```json
{"type":"hello","version":1,"features":{"mcp":true,"glyph_push":false},"transport":"websocket","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}
{"session_id":"<id>","type":"listen","state":"start","mode":"auto"}
{"session_id":"<id>","type":"listen","state":"stop"}
{"session_id":"<id>","type":"listen","state":"detect","text":"hi esp"}
{"session_id":"<id>","type":"abort","reason":"wake_word_detected"}
```

El primer mensaje del servidor DEBE ser un hello con `transport:"websocket"` en < 10 s.

---

## 4. Que cambiar de cada lado

### A. GATEWAY (obligatorio antes del viernes): endpoint OTA

`POST /xiaozhi/ota/` (sin auth; el firmware lo llama por HTTP plano). Identificar el aparato por header `Device-Id` (MAC). Respuesta 200:

```json
{
  "server_time": {"timestamp": 1730000000000, "timezone_offset": -360},
  "websocket": {"url": "ws://192.168.1.50:8000/ws/device", "token": "<device_key>", "version": 1},
  "activation": {"code": "482913", "message": "Codigo de vinculacion\n482913", "challenge": "<cadena aleatoria>", "timeout_ms": 30000}
}
```

Reglas:
- `websocket` DEBE ir siempre (si falta, el firmware usa MQTT). `version` debe ser 1, **nunca 0** (0 = "no definido"). La URL puede llevar `?token=` pero no hace falta.
- `activation` solo mientras el aparato no esta vinculado a un paciente. `code` = el `pairing_code` del device (6 digitos; se dicen con voz es-ES). `message` se pinta en pantalla tal cual (usar `\n`).
- No incluir `firmware` (o con `version` igual a la que reporta el aparato en `application.version`, hoy `2.5.3`, y `url` vacia); una version mayor dispara descarga OTA.
- `token`: crear/buscar el device por MAC (`repo.create_device` con key aleatoria o derivada, >= 8 chars). Sugerencia de seguridad: el firmware **conserva en NVS** las claves de `websocket` que ya recibio y solo sobrescribe las que llegan; por tanto entregar `token` solo en el primer contacto/mientras no esta vinculado y omitirlo despues evita que alguien en la LAN que falsee la MAC robe la key de un aparato ya vinculado. (Hay que probarlo en placa.)
- Alternativa minima para hoy, sin tocar el flujo de vinculacion: con `ALLOW_DEVICE_AUTOREGISTER=true` el handshake WS ya crea el device, pero el codigo solo llegaria via OTA de todos modos.

### B. GATEWAY: `POST /xiaozhi/ota/activate`

El firmware lo llama (URL OTA + `activate`) en bucle cuando hay `activation.challenge`: responder **202** mientras no este vinculado (el aparato reintenta cada 3 s) y **200** cuando ya lo esta. Con 200 el aparato vuelve a llamar al OTA, ya sin `activation`, y continua a modo normal. Body entrante: `{}` (activacion v1, sin numero de serie).
Si NO se manda `challenge`, el aparato igual muestra el codigo pero hace 10 reintentos de 10 s fallidos antes de re-consultar OTA (~100 s de latencia para pasar a normal). Mandar siempre `challenge`.

### C. Flujo de vinculacion resultante

1. Aparato conecta Wi-Fi -> `POST /xiaozhi/ota/` -> recibe `activation.code`.
2. Pantalla muestra "Codigo de vinculacion 482913" y lo dice en voz alta.
3. Cuidador reclama el codigo en el dashboard (`POST /devices/claim`, ya existe).
4. `POST /xiaozhi/ota/activate` pasa de 202 a 200 -> el aparato re-consulta OTA -> sin `activation` -> modo normal (Idle). La primera conversacion abre el WS.

### D. FIRMWARE (parche pendiente, NO hecho): modo proactivo

Para que Kylie hable sin que nadie la active hace falta **canal WS persistente en reposo**. Estado actual: `WebsocketProtocol::Start()` no conecta y el canal se abre solo en `Application::ContinueOpenAudioChannel` / `ContinueWakeWordInvoke`. Plan de parche (rama `kylie`, en `main/application.cc` + `main/protocols/websocket_protocol.cc`):

1. Tras `MAIN_EVENT_ACTIVATION_DONE`, llamar `protocol_->OpenAudioChannel()` en estado Idle y reabrir con backoff si `OnAudioChannelClosed` dispara. Hoy `OnAudioChannelClosed` fuerza `SetDeviceState(Idle)` y apaga `PowerSaveLevel::PERFORMANCE`; mantenerlo pero reprogramar la reconexion.
2. El handler `tts start` ya hace `SetDeviceState(Speaking)` desde cualquier estado y `OnIncomingAudio` acepta audio en Speaking, asi que el servidor puede empujar `tts start` + frames + `tts stop` sobre el canal abierto sin tipo `wake`. La maquina de estados ya permite Idle->Speaking (`device_state_machine.cc`); falta que al terminar vuelva a Idle (en `tts stop` con modo auto el firmware pasa a Listening y abre el microfono: para proactivo preferible volver a Idle; hay que decidirlo).
3. Gateway: keepalive JSON cada 30-45 s (ver timeout de 120 s en 3.2).
4. Opcional en el mismo parche: aceptar `{"type":"wake"}` y `{"type":"display",...}` en `OnIncomingJson`; enviar `{"type":"state","battery":..,"charging":..}` periodico (la placa tiene `PowerManager` con nivel de bateria en `power_manager.h`).
5. Consumo: canal abierto permanente aumenta gasto de bateria; el sleep de la placa (`PowerSaveTimer(-1,60,290)`: pantalla off a 60 s, deep sleep a 290 s sin cargar) **apagaria el aparato a los ~5 min** y mataria el modo proactivo; desactivar o relajar el shutdown cuando el canal persistente este activo o el aparato este cargando.

Para el viernes (sin parche) el aparato solo funciona en modo reactivo: toque/BOOT/"Hi ESP" -> conversacion.

---

## 5. Configurar la URL del servidor propio (resumen)

| Metodo | Como | Cuando |
|---|---|---|
| Compilacion | `KYLIE_HOST=<ip> ./scripts/kylie-build.sh` -> `CONFIG_OTA_URL="http://<ip>:8000/xiaozhi/ota/"` | Valor por defecto del binario |
| Pagina de config Wi-Fi | AP `Xiaozhi-XXXX` -> `http://192.168.4.1` -> campo "URL OTA personalizada" (visible porque `show_ota_config=true`); se guarda en NVS `wifi/ota_url` y gana sobre el compilado | Cambiar IP sin recompilar |
| NVS directo | namespace `wifi`, key `ota_url` (string) | Avanzado |
| URL del WebSocket y token | **No se configuran en el aparato**: salen de la respuesta OTA (`websocket.url`, `websocket.token`) y se guardan en NVS namespace `websocket` (`url`, `token`, `version`) | Siempre via servidor |

Para forzar rehacer todo: `idf.py -p PORT erase-flash` y reflashear (borra NVS: Wi-Fi, token, UUID).

---

## 6. Guia de flasheo del viernes (placa fisica)

Requisitos previos (hacer ANTES, con el Mac en la misma red Wi-Fi 2.4 GHz que usara el aparato):
1. Gateway con `POST /xiaozhi/ota/` y `/xiaozhi/ota/activate` (seccion 4) y Docker publicando el puerto 8000 en `0.0.0.0`. Permitir conexiones entrantes en el firewall de macOS si pregunta.
2. IP del Mac: `ipconfig getifaddr en0`. Probar desde otro dispositivo: `curl -X POST http://<ip>:8000/xiaozhi/ota/ -H 'Device-Id: AA:BB:CC:DD:EE:FF'` debe dar 200 con `websocket`.
3. Recompilar con la IP real: `KYLIE_HOST=<ip> ./scripts/kylie-build.sh` (o saltarse y usar el campo de URL OTA en el paso 6).

Pasos:
1. **Conectar** la placa por USB-C al Mac (cable de datos). Con bateria puesta esta bien; encender si tiene interruptor.
2. **Puerto**: `ls /dev/cu.usbmodem*` (USB nativo del ESP32-S3). Si no aparece ninguno prueba `ls /dev/cu.usb*` (algunos modelos traen puente CH340/CP210x: `/dev/cu.wchusbserial*` / `/dev/cu.usbserial*`). Llamaremos `PORT` a la ruta.
3. **Si no entra a flashear** (error "Failed to connect" / no responde): modo descarga = mantener pulsado **BOOT** (GPIO0), pulsar y soltar **RESET** (o desenchufar y enchufar el USB manteniendo BOOT), soltar BOOT. El puerto puede cambiar de nombre en modo descarga; volver a hacer `ls`. Tras flashear, pulsar RESET para arrancar normal.
4. **Flashear + monitor** (entorno IDF cargado):
   ```sh
   source ~/esp/esp-idf/export.sh
   cd ~/Documents/kylie-firmware
   idf.py -p PORT flash monitor          # salir del monitor: Ctrl+]
   ```
   Alternativa con un solo binario fusionado:
   ```sh
   python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x0 build/merged-binary.bin
   idf.py -p PORT monitor
   ```
   Offsets equivalentes: `0x0 bootloader.bin`, `0x8000 partition-table.bin`, `0xd000 ota_data_initial.bin`, `0x20000 xiaozhi.bin`, `0x800000 generated_assets.bin` (flash 16 MB, dio, 80 MHz). Si `idf.py` se queja del directorio build (se compilo otra variante despues), volver a ejecutar `./scripts/kylie-build.sh`.
5. **Primer arranque / Wi-Fi**: sin SSID guardado el aparato entra solo en modo configuracion y muestra "Modo configuracion Wi-Fi" con el nombre del hotspot. Desde Mac/telefono: conectarse al Wi-Fi abierto `Xiaozhi-XXXX` (XXXX = ultimos 2 bytes de la MAC), abrir `http://192.168.4.1` (suele salir el portal cautivo). Elegir la red **2.4 GHz** (no 5 GHz), poner la clave, y si no recompilaste con la IP real, rellenar "URL OTA personalizada" = `http://<ip-mac>:8000/xiaozhi/ota/`. Guardar; el aparato se conecta. Para volver a este modo mas tarde: pulsar BOOT o tocar la pantalla mientras esta arrancando ("Iniciando"), o borrar flash.
6. **Verificar conexion al servidor**:
   - Monitor serie: `Current version: 2.5.3`, luego la peticion OTA. En los logs del servidor debe verse `POST /xiaozhi/ota/ 200`.
   - El aparato debe mostrar **"Codigo de vinculacion" + 6 digitos** y decirlos en voz alta. Si en cambio muestra "Error ... reintentando en 10 s": el OTA no responde (IP errada, firewall, endpoint ausente).
   - Reclamar el codigo desde el dashboard; el aparato pasa a reposo (cara neutral, "En espera").
   - Tocar la pantalla (toque corto) o pulsar BOOT, o decir "Hi ESP": log `Connecting to websocket server: ws://<ip>:8000/ws/device`, luego `Session ID: ...`; hablar y esperar stt/tts.
7. Si tras vincular no abre WS: revisar en el log del gateway el handshake (`Authorization` presente, key >= 8 chars) y que el hello servidor lleve `transport:"websocket"`.

---

## 7. Mantenimiento del fork

```sh
git remote add upstream https://github.com/78/xiaozhi-esp32.git
git fetch --unshallow origin && git fetch upstream
git checkout main && git merge --ff-only upstream/main && git push origin main
git checkout kylie && git rebase main            # nuestros cambios son solo archivos nuevos
```

El clon local es superficial (`--depth 1`); `--unshallow` antes de integrar upstream. Tras cambiar de revision de ESP-IDF: `git submodule update --init --recursive` en `~/esp/esp-idf`, `source export.sh` y `idf.py fullclean`. Reglas del proyecto en `AGENTS.md`: no editar `sdkconfig*`, `managed_components/`, `build/`; no cambiar pines de una placa existente (identidad de board afecta OTA).
