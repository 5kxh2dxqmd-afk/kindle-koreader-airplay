#!/bin/sh
# Name: Kindle AirPlay
# Author: Kindle AirPlay
# DontUseFBInk

# IllusionGeneric-style launcher: copy the WAF into /var/local/mesquite,
# register it in appreg.db with a file:// command path, start the receiver
# daemon, then ask appmgrd to open the app.

APP_ID="com.kindleairplay.app"
SOURCE_DIR="/mnt/us/documents/illusion-airplay"
TARGET_DIR="/var/local/mesquite/${APP_ID}"
DB="/var/local/appreg.db"
DAEMON="${SOURCE_DIR}/airplay_ill"
LOG="${SOURCE_DIR}/receiver.log"

if [ ! -d "$SOURCE_DIR" ] || [ ! -f "${SOURCE_DIR}/index.html" ] || [ ! -f "${SOURCE_DIR}/config.xml" ]; then
    echo "AirPlay app files are missing. Copy the complete illusion-airplay folder to documents."
    exit 1
fi

chmod +x "$DAEMON" 2>/dev/null || true

# Fresh mesquite tree every launch so config.xml / assets always match the zip.
if [ -d "$TARGET_DIR" ]; then
    rm -rf "$TARGET_DIR"
fi
cp -r "$SOURCE_DIR" "$TARGET_DIR"

# Receiver serves /api/* for the WAF. UI itself loads from local files.
if ! pidof airplay_ill >/dev/null 2>&1; then
    if [ -x "$DAEMON" ]; then
        nohup "$DAEMON" "$SOURCE_DIR" >"$LOG" 2>&1 </dev/null &
        sleep 2
    else
        echo "airplay_ill is missing or not executable" >"$LOG"
    fi
fi

sqlite3 "$DB" <<EOF
INSERT OR IGNORE INTO interfaces(interface) VALUES('application');
INSERT OR IGNORE INTO handlerIds(handlerId) VALUES('${APP_ID}');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','lipcId','${APP_ID}');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','command','/usr/bin/mesquite -l ${APP_ID} -c file://${TARGET_DIR}/');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('${APP_ID}','supportedOrientation','U');
EOF

sleep 1
nohup lipc-set-prop com.lab126.appmgrd start "app://${APP_ID}" >/dev/null 2>&1 &
