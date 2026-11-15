#!/bin/sh
# Restore drill (D9): bring the whole service back on a machine that never ran
# it, from nothing but the offsite copies, the decryption key and the code.
#
# restore-test.sh, every 1st of the month, proves the database decrypts and
# opens. This proves the step after: that the server starts on it, serves the
# accounts, the history and the attachments, and somebody can sign in. It runs
# on the offsite machine itself (the one place the backups are), next to and
# never instead of whatever already runs there:
#
#   restore-drill.sh <identity file> <harmony binary for this machine> [work dir]
#
# It listens on 127.0.0.1 only, with voice off, and is reached through an SSH
# tunnel. Nothing here touches the live service. When it is done:
#   pkill -f "$WORK/harmony" ; shred -u "$WORK"/data/harmony.db* ; rm -rf "$WORK"
set -eu

IDENTITY="$1"
BIN="$2"
WORK="${3:-/tmp/harmony-drill}"
PORT="${PORT:-18080}"
BACKUPS="${BACKUPS:-$HOME/harmony-backups}"
FILES="${FILES:-$HOME/harmony-files}"

umask 077
mkdir -p "$WORK/data/files" "$WORK/data/app"

newest=$(ls -1t "$BACKUPS"/harmony-*.db.age | head -1)
echo "backup   $newest"
if [ "$(sha256sum "$newest" | awk '{print $1}')" != "$(cat "$newest.sha256")" ]; then
	echo "the backup does not match its checksum" >&2
	exit 1
fi
age -d -i "$IDENTITY" -o "$WORK/data/harmony.db" "$newest"

n=0
for f in "$FILES"/*.age; do
	[ -e "$f" ] || continue
	age -d -i "$IDENTITY" -o "$WORK/data/files/$(basename "$f" .age)" "$f"
	n=$((n + 1))
done
echo "files    $n attachment(s) decrypted"

cp "$BIN" "$WORK/harmony"
chmod 700 "$WORK/harmony"
"$WORK/harmony" -verify "$WORK/data/harmony.db"

echo "starting on 127.0.0.1:$PORT (voice off)"
HARMONY_DB="$WORK/data/harmony.db" \
HARMONY_FILES="$WORK/data/files" \
HARMONY_APP_DIR="$WORK/data/app" \
HARMONY_ADDR="127.0.0.1:$PORT" \
HARMONY_BASE_URL="http://localhost:$PORT" \
HARMONY_INSECURE_COOKIE=1 \
HARMONY_VOICE_PORT=0 \
	nohup "$WORK/harmony" >"$WORK/server.log" 2>&1 &
sleep 2
curl -s -o /dev/null -w "healthz  %{http_code}\n" "http://127.0.0.1:$PORT/healthz"
