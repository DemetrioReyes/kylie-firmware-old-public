#!/usr/bin/env bash
# KylieAI: build del firmware para Spotpear ESP32-S3-Touch-LCD-1.28 BOX apuntando a nuestro servidor.
#
# Uso:
#   KYLIE_HOST=192.168.1.50 ./scripts/kylie-build.sh          # build con la IP real del servidor
#   ./scripts/kylie-build.sh                                   # build con placeholder (192.168.PLACEHOLDER)
#
# Variables:
#   KYLIE_HOST   host/IP del servidor KylieAI (default: 192.168.PLACEHOLDER)
#   KYLIE_PORT   puerto HTTP del gateway   (default: 8000)
#   KYLIE_SCHEME http | https              (default: http)
#   KYLIE_LANG   locale del firmware       (default: es-ES)
#   IDF_PATH     ruta de ESP-IDF           (default: ~/esp/esp-idf)
#
# El dispositivo SOLO conoce la URL OTA (CONFIG_OTA_URL). La URL del WebSocket y el token los entrega
# el servidor en la respuesta de OTA (ver KYLIE-NOTES.md). Tambien se puede cambiar la URL OTA en
# caliente desde la pagina de configuracion Wi-Fi del aparato (campo "OTA URL"), sin recompilar.
set -euo pipefail

KYLIE_HOST="${KYLIE_HOST:-192.168.PLACEHOLDER}"
KYLIE_PORT="${KYLIE_PORT:-8000}"
KYLIE_SCHEME="${KYLIE_SCHEME:-http}"
KYLIE_LANG="${KYLIE_LANG:-es-ES}"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
BOARD_DIR="spotpear/sp-esp32-s3-1.28-box"
VARIANT="sp-esp32-s3-1.28-box-kylie"
OTA_URL="${KYLIE_SCHEME}://${KYLIE_HOST}:${KYLIE_PORT}/xiaozhi/ota/"

cd "$(dirname "$0")/.."

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

# Genera la config propia de la variante (no se toca el config.json original de la placa).
cat > "main/boards/${BOARD_DIR}/kylie.json" <<JSON
{
    "manufacturer": "spotpear",
    "type": "sp-esp32-s3-1.28-box",
    "target": "esp32s3",
    "builds": [
        {
            "name": "${VARIANT}",
            "sdkconfig_append": [
                "CONFIG_OTA_URL=\"${OTA_URL}\""
            ]
        }
    ]
}
JSON

echo ">> OTA URL compilada: ${OTA_URL}"
python3 scripts/build.py -c kylie.json "${BOARD_DIR}" --name "${VARIANT}" --language "${KYLIE_LANG}"
