#!/bin/sh
# installed as /opt/audiolaz/audiolaz-receiver beside audiolaz-recv and its settings, receiver.conf (written by the app:
# Started, it waits: it opens the DAC only when a stream comes and gives it back at the stream's end. Nothing else on the
# they play (the receiver asks for it 2.5 s while the other lets it go, and is refused by the card while it is held).
# never above another program's real time, a guard that backs off before the machine falls).
#   run     in the foreground (the systemd unit audiolaz-link.service: ExecStart=/opt/audiolaz/audiolaz-receiver run)
#   start   the unit when there is one, else in the background (pid in /tmp/audiolaz-recv.pid, log /tmp/audiolaz-recv.log)
#   stop    the unit, else SIGTERM (the DAC given back cleanly by the receiver), then waits up to 3 s
# The player brings it back when it does not answer and this machine forgot it (a machine that runs from RAM):
D=$(cd "$(dirname "$0")" && pwd)
CONF=$D/receiver.conf
PIDF=/tmp/audiolaz-recv.pid
LOG=/tmp/audiolaz-recv.log
UNIT=audiolaz-link.service
DAC=auto NAME= ID= MAXRATE=384000 EXTRA=
[ -f "$CONF" ] && . "$CONF"
OPTS="--max-rate $MAXRATE --period-us 10000 --periods 8 --ring-ms 100 --ring-max-ms 500 --stats $EXTRA"
unit() { [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1 && [ -f /etc/systemd/system/$UNIT ]; }
running() { [ -f $PIDF ] && kill -0 "$(cat $PIDF)" 2>/dev/null; }
case "$1" in
  run)
    if [ -n "$NAME" ]; then set -- --name "$NAME"; else set --; fi
    [ -n "$ID" ] && set -- "$@" --id "$ID"
    # shellcheck disable=SC2086
    exec "$D/audiolaz-recv" --dac "$DAC" "$@" $OPTS ;;
  start)
    if unit; then systemctl start $UNIT; sleep 1; systemctl is-active $UNIT; exit 0; fi
    running && { echo "already running"; exit 0; }
    nohup "$D/audiolaz-receiver" run > $LOG 2>&1 < /dev/null &
    echo $! > $PIDF; sleep 1; tail -3 $LOG ;;
  stop)
    if unit; then systemctl stop $UNIT; echo stopped; exit 0; fi
    if running; then P=$(cat $PIDF); kill "$P"; i=0; while kill -0 "$P" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done; fi
    rm -f $PIDF; echo stopped ;;
  status)
    if unit; then
      P=$(systemctl show -p MainPID --value $UNIT 2>/dev/null)
      if systemctl is-active --quiet $UNIT && [ -n "$P" ] && [ "$P" != 0 ]; then echo "running (pid $P)"; else echo "not running"; fi
      journalctl -u $UNIT -n 8 --no-pager -o cat 2>/dev/null
    else
      running && echo "running (pid $(cat $PIDF))" || echo "not running"
      tail -8 $LOG 2>/dev/null
    fi ;;
  *) echo "usage: $0 run|start|stop|status"; exit 2 ;;
esac
