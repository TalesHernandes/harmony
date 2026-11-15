#!/bin/sh
# Daily backup of the harmony database: a consistent copy, encrypted, sent to a
# different machine at a different provider.
#
# Three things make this a backup rather than a file copy:
#
#   * the copy is made by the server itself (`harmony -backup`), which runs
#     SQLite's integrity check and then VACUUM INTO. `cp` on a live SQLite file
#     can capture a half-written page and nobody finds out until the restore;
#   * it is encrypted with age before it leaves this machine, so the copy
#     sitting on the other server is not readable there;
#   * it goes somewhere else. A backup on the same disk protects against a
#     mistake, not against losing the disk.
#
# And since 24/09/2026 the attachments go too, each one encrypted on its own.
# Until then this copied the database alone, and when the service moved off
# the Contabo on 18/09/2026 the upload folder did not come with it: nine
# pictures and files became a name with nothing behind it (docs/DEPLOY.md
# §38). An attachment never changes once uploaded, so only the new ones are
# sent each day.
#
# The restore is tested by restore-test.sh, on the same schedule as this.
set -eu

APP_DIR="${HARMONY_DIR:-/srv/harmony}"
COMPOSE="docker compose -f $APP_DIR/docker-compose.harmony.yml"
DATA_DIR="$APP_DIR/data"
OUT_DIR="$APP_DIR/backups"
RECIPIENTS="/etc/harmony/backup-recipients.txt"
SSH_KEY="/etc/harmony/offsite_ed25519"
REMOTE="${HARMONY_OFFSITE:-usuario@maquina-do-backup}"
REMOTE_DIR="harmony-backups"
REMOTE_FILES="harmony-files"
FILES_DIR="$DATA_DIR/files"
KEEP_LOCAL=14
KEEP_REMOTE=30

stamp=$(date -u +%Y%m%dT%H%M%SZ)
stage="$DATA_DIR/backup.db"
out="$OUT_DIR/harmony-$stamp.db.age"

mkdir -p "$OUT_DIR"
sealed="$DATA_DIR/backup-files"
# A failed run must not leave a plaintext copy of the whole service lying about.
trap 'rm -f "$stage" "$sealed.there" "$sealed.known"; rm -rf "$sealed"' EXIT INT TERM

# --no-deps so this never starts Caddy, and works whether or not the service is
# currently up.
$COMPOSE run --rm --no-deps harmony -backup /data/backup.db
age -R "$RECIPIENTS" -o "$out" "$stage"
# Which attachments the database knows, read from the consistent copy rather
# than the live file. This list, and not the folder, decides what the other
# machine keeps: see the sync below.
known=$(sqlite3 -readonly "$stage" "SELECT id FROM attachments" | sort)
rm -f "$stage"

sha256sum "$out" | awk '{print $1}' > "$out.sha256"
echo "backup  $out ($(stat -c %s "$out") bytes)"

ssh -i "$SSH_KEY" -o StrictHostKeyChecking=yes "$REMOTE" "mkdir -p $REMOTE_DIR"
scp -q -i "$SSH_KEY" -o StrictHostKeyChecking=yes "$out" "$out.sha256" "$REMOTE:$REMOTE_DIR/"

# Confirm the copy that arrived is the copy that left. Without this the offsite
# file could be truncated and the log would still say "sent".
remote_sum=$(ssh -i "$SSH_KEY" "$REMOTE" "sha256sum $REMOTE_DIR/$(basename "$out")" | awk '{print $1}')
if [ "$remote_sum" != "$(cat "$out.sha256")" ]; then
	echo "offsite copy does not match the local checksum" >&2
	exit 1
fi
echo "offsite $REMOTE:$REMOTE_DIR/$(basename "$out") verified"

# The attachments. Each is sealed on its own as <id>.age, so a day's run sends
# only what is new, and a restore can bring back one picture without the rest.
ssh -i "$SSH_KEY" "$REMOTE" "mkdir -p $REMOTE_FILES"
there=$(ssh -i "$SSH_KEY" "$REMOTE" "ls -1 $REMOTE_FILES" | sed -n 's/\.age$//p' | sort)
mkdir -p "$sealed"
sent=0
for id in $known; do
	# Already there, or not on this disk (lost here is exactly when the copy
	# there matters, so it is never replaced by nothing).
	if printf '%s\n' "$there" | grep -qxF -e "$id" || [ ! -f "$FILES_DIR/$id" ]; then
		continue
	fi
	age -R "$RECIPIENTS" -o "$sealed/$id.age" "$FILES_DIR/$id"
	sent=$((sent + 1))
done
if [ "$sent" -gt 0 ]; then
	scp -q -i "$SSH_KEY" -o StrictHostKeyChecking=yes "$sealed"/*.age "$REMOTE:$REMOTE_FILES/"
fi
rm -rf "$sealed"

# What goes away there is only what the database no longer knows: a message
# removed, an upload never posted. A file missing from THIS disk never deletes
# its copy — a backup that mirrors a loss is not a backup.
printf '%s\n' "$there" | sed '/^$/d' > "$sealed.there"
printf '%s\n' "$known" | sed '/^$/d' > "$sealed.known"
gone=$(comm -23 "$sealed.there" "$sealed.known")
rm -f "$sealed.there" "$sealed.known"
# An empty database list would read as "everything is gone" and wipe the copy.
# A service with messages always has some attachments; refuse rather than guess.
if [ -z "$known" ] && [ -n "$there" ]; then
	echo "the database lists no attachments but the other machine has some; not deleting" >&2
	gone=""
fi
for id in $gone; do
	ssh -i "$SSH_KEY" "$REMOTE" "rm -f $REMOTE_FILES/$id.age"
done

# And the check that makes it a backup: everything the database knows and
# this disk has is on the other machine now.
there=$(ssh -i "$SSH_KEY" "$REMOTE" "ls -1 $REMOTE_FILES" | sed -n 's/\.age$//p' | sort)
missing=0
for id in $known; do
	[ -f "$FILES_DIR/$id" ] || continue
	printf '%s\n' "$there" | grep -qxF -e "$id" || missing=$((missing + 1))
done
if [ "$missing" -gt 0 ]; then
	echo "$missing attachment(s) did not reach the other machine" >&2
	exit 1
fi
echo "offsite anexos: $sent novos, $(printf '%s\n' "$gone" | sed '/^$/d' | wc -l) removidos, $(printf '%s\n' "$there" | sed '/^$/d' | wc -l) guardados"

# Keep a fortnight here and a month there. Older copies of a service this size
# are not worth the disk, and a very old database would restore a membership
# list that no longer matches anybody's password.
ls -1t "$OUT_DIR"/harmony-*.db.age 2>/dev/null | tail -n +$((KEEP_LOCAL + 1)) | while read -r old; do
	rm -f "$old" "$old.sha256"
done
ssh -i "$SSH_KEY" "$REMOTE" \
	"ls -1t $REMOTE_DIR/harmony-*.db.age 2>/dev/null | tail -n +$((KEEP_REMOTE + 1)) | while read -r old; do rm -f \"\$old\" \"\$old.sha256\"; done"
