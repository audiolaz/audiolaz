#!/bin/sh
# audiolaz-receiver-up <address> [port] : run by the player (netout.c, as /usr/local/lib/audiolaz/audiolaz-receiver-up,
# it is copied there again from this machine's staging folder and started (idle: it takes the DAC only when a stream
#   the staging folder holds audiolaz-recv (armv7) and audiolaz-receiver.sh (engine/alsa/)
# the receivers the app installs (src/linkInstall.js, a machine without systemd): each one's files in a folder
# of its own, $STAGE/<address>/ (audiolaz-recv for its processor, audiolaz-receiver.sh = engine/linux/audiolaz-link.sh,
# receivers (link-key, put on the receiver by the app; its host key kept in link-known_hosts), when there is one (else
H=${1:?address}
STAGE=${AUDIOLAZ_RECEIVER_STAGE:-/opt/audiolaz-receiver-staging}
[ -d "$STAGE/$H" ] && STAGE=$STAGE/$H
K=/usr/local/lib/audiolaz/link-key
OPT=
[ -f "$K" ] && OPT="-i $K -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/usr/local/lib/audiolaz/link-known_hosts"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=3 $OPT root@$H"
exec >> /tmp/audiolaz-receiver-up.log 2>&1
echo "$(date '+%F %T') $H: bringing the receiver up"
if ! $SSH '[ -x /opt/audiolaz/audiolaz-recv ] && [ -x /opt/audiolaz/audiolaz-receiver ]'; then
  [ -f "$STAGE/audiolaz-recv" ] && [ -f "$STAGE/audiolaz-receiver.sh" ] || { echo "nothing staged in $STAGE"; exit 1; }
  $SSH 'mkdir -p /opt/audiolaz' &&
  $SSH 'cat > /opt/audiolaz/audiolaz-recv && chmod 755 /opt/audiolaz/audiolaz-recv' < "$STAGE/audiolaz-recv" &&
  $SSH 'cat > /opt/audiolaz/audiolaz-receiver && chmod 755 /opt/audiolaz/audiolaz-receiver' < "$STAGE/audiolaz-receiver.sh" || { echo "copy failed"; exit 1; }
  if [ -f "$STAGE/receiver.conf" ]; then
    $SSH 'cat > /opt/audiolaz/receiver.conf && : > /opt/audiolaz/.audiolaz-link' < "$STAGE/receiver.conf" || { echo "copy failed"; exit 1; }
  fi
  echo "copied from $STAGE"
fi
$SSH '/opt/audiolaz/audiolaz-receiver start'
