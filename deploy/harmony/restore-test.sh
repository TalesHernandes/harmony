#!/bin/sh
# Restore the newest offsite backup and check that what comes back is a working
# database with the accounts still in it.
#
# PLANO-DISCORD.md §5: "backup que nunca foi restaurado não é backup". This runs
# the whole way round — fetch from the other machine, decrypt, open, integrity
# check, count the accounts — so the failure that gets found is found by a timer
# on a Tuesday and not on the worst day of the year.
#
# It reads only. Nothing here touches the live database.
set -eu

APP_DIR="${HARMONY_DIR:-/srv/harmony}"
COMPOSE="docker compose -f $APP_DIR/docker-compose.harmony.yml"
DATA_DIR="$APP_DIR/data"
IDENTITY="/etc/harmony/backup-identity.txt"
SSH_KEY="/etc/harmony/offsite_ed25519"
REMOTE="${HARMONY_OFFSITE:-usuario@maquina-do-backup}"
REMOTE_DIR="harmony-backups"
REMOTE_FILES="harmony-files"

work="$DATA_DIR/restore-check"
trap 'rm -f "$work.db.age" "$work.db" "$work.db-wal" "$work.db-shm" "$work.sha256" "$work.file.age" "$work.file"' EXIT INT TERM

newest=$(ssh -i "$SSH_KEY" "$REMOTE" "ls -1t $REMOTE_DIR/harmony-*.db.age 2>/dev/null | head -1")
if [ -z "$newest" ]; then
	echo "no backup found at $REMOTE:$REMOTE_DIR" >&2
	exit 1
fi
echo "restoring $newest"

scp -q -i "$SSH_KEY" "$REMOTE:$newest" "$work.db.age"
scp -q -i "$SSH_KEY" "$REMOTE:$newest.sha256" "$work.sha256"
if [ "$(sha256sum "$work.db.age" | awk '{print $1}')" != "$(cat "$work.sha256")" ]; then
	echo "the file that came back does not match its checksum" >&2
	exit 1
fi

age -d -i "$IDENTITY" -o "$work.db" "$work.db.age"

# The server checks its own restore: same binary, same schema, same integrity
# rules as the running service. It exits non-zero if the file is corrupt or if
# it opens but holds no accounts, which is the failure a naive check misses.
$COMPOSE run --rm --no-deps harmony -verify /data/restore-check.db

# And one attachment, drawn at random, all the way back: fetched, decrypted,
# and compared with the file the service holds. A key that encrypts but was
# never tried for decrypting is found here, not on the day it is needed.
pick=$(ssh -i "$SSH_KEY" "$REMOTE" "ls -1 $REMOTE_FILES 2>/dev/null" | shuf -n 1)
if [ -n "$pick" ]; then
	id=${pick%.age}
	scp -q -i "$SSH_KEY" "$REMOTE:$REMOTE_FILES/$pick" "$work.file.age"
	age -d -i "$IDENTITY" -o "$work.file" "$work.file.age"
	if [ -f "$DATA_DIR/files/$id" ] && ! cmp -s "$work.file" "$DATA_DIR/files/$id"; then
		echo "attachment $id came back different from the one in service" >&2
		exit 1
	fi
	echo "attachment $id restored ($(stat -c %s "$work.file") bytes)"
fi

echo "restore test passed"
