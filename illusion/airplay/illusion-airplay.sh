#!/bin/sh
# Name: Kindle AirPlay
# Author: Kindle AirPlay
# DontUseFBInk

set -eu

APP_ID="org.kindle.airplay"
SOURCE_DIR="/mnt/us/documents/illusion-airplay"
TARGET_DIR="/var/local/mesquite/${APP_ID}"
DB="/var/local/appreg.db"
DAEMON="${SOURCE_DIR}/airplay_ill"

if [ ! -x "$DAEMON" ] || [ ! -f "${SOURCE_DIR}/index.html" ]; then
    echo "AirPlay app files are missing. Copy the complete illusion-airplay folder to documents."
    exit 1
fi

mkdir -p "$TARGET_DIR"
cp "${SOURCE_DIR}/config.xml" "$TARGET_DIR/config.xml"

# Start one local web/AirPlay receiver process. It serves this folder to Mesquite.
if ! pidof airplay_ill >/dev/null 2>&1; then
    nohup "$DAEMON" "$SOURCE_DIR" >"${SOURCE_DIR}/receiver.log" 2>&1 </dev/null &
    sleep 2
fi

if ! pidof airplay_ill >/dev/null 2>&1; then
    echo "Could not start the AirPlay receiver. Check receiver.log."
    exit 1
fi

sqlite3 "$DB" <<EOF
INSERT OR IGNORE INTO interfaces(interface) VALUES('application');
INSERT OR IGNORE INTO handlerIds(handlerId) VALUES('${APP_ID}');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','lipcId','${APP_ID}');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','command','/usr/bin/mesquite -l ${APP_ID} -c http://127.0.0.1:7080/');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','supportedOrientation','U');
EOF

sleep 1
nohup lipc-set-prop com.lab126.appmgrd start "app://${APP_ID}" >/dev/null 2>&1 &
