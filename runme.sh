#!/bin/sh
set -eu

RESULT="result.txt"
WORKDIR="$(pwd)/test_runtime"
LOG="/tmp/myinit.log"
PIDFILE="/tmp/myinit.pid"
MYINIT="$(pwd)/myinit"

write_result() {
    printf '%s\n' "$*" >> "$RESULT"
}

cleanup() {
    if [ -f "$PIDFILE" ]; then
        pid="$(cat "$PIDFILE" 2>/dev/null || true)"
        if [ -n "${pid:-}" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    fi
    pkill -f "$WORKDIR/proc1" 2>/dev/null || true
    pkill -f "$WORKDIR/proc2" 2>/dev/null || true
    pkill -f "$WORKDIR/proc3" 2>/dev/null || true
}

child_count() {
    pid="$1"
    ps -eo ppid=,args= | awk -v p="$pid" -v w="$WORKDIR" '$1 == p && $0 ~ w"/proc[123]( |$)" { count++ } END { print count + 0 }'
}

show_children() {
    pid="$1"
    ps -eo pid=,ppid=,args= | awk -v p="$pid" -v w="$WORKDIR" '$2 == p && $0 ~ w"/proc[123]( |$)"'
}

trap cleanup EXIT INT TERM

rm -f "$RESULT"
make clean >/dev/null 2>&1 || true
write_result "Тесты myinit"
write_result "Ожидается: сборка через make, запуск демона, три дочерних процесса, рестарт убитого процесса, перезагрузка конфигурации по SIGHUP до одного процесса."
write_result ""

make >> "$RESULT" 2>&1

rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
rm -f "$LOG" "$PIDFILE"

cat > "$WORKDIR/in1" <<EOF_IN
input 1
EOF_IN
cat > "$WORKDIR/in2" <<EOF_IN
input 2
EOF_IN
cat > "$WORKDIR/in3" <<EOF_IN
input 3
EOF_IN

ln -s /bin/sleep "$WORKDIR/proc1"
ln -s /bin/sleep "$WORKDIR/proc2"
ln -s /bin/sleep "$WORKDIR/proc3"

cat > "$WORKDIR/config3.conf" <<EOF_CONF
$WORKDIR/proc1 3000 $WORKDIR/in1 $WORKDIR/out1
$WORKDIR/proc2 3000 $WORKDIR/in2 $WORKDIR/out2
$WORKDIR/proc3 3000 $WORKDIR/in3 $WORKDIR/out3
EOF_CONF

cat > "$WORKDIR/config1.conf" <<EOF_CONF
$WORKDIR/proc1 3000 $WORKDIR/in1 $WORKDIR/out1
EOF_CONF

cp "$WORKDIR/config3.conf" "$WORKDIR/current.conf"
"$MYINIT" -c "$WORKDIR/current.conf"

i=0
while [ ! -f "$PIDFILE" ] && [ "$i" -lt 30 ]; do
    i=$((i + 1))
    sleep 1
done

if [ ! -f "$PIDFILE" ]; then
    write_result "Фактически: FAIL, файл $PIDFILE не создан."
    exit 1
fi

daemon_pid="$(cat "$PIDFILE")"
sleep 1

write_result "Тест 1: после старта должно быть 3 дочерних процесса."
count="$(child_count "$daemon_pid")"
write_result "Фактически найдено процессов: $count"
show_children "$daemon_pid" >> "$RESULT"
if [ "$count" -ne 3 ]; then
    write_result "Итог: FAIL"
    exit 1
fi
write_result "Итог: OK"
write_result ""

write_result "Тест 2: убить процесс номер 2 через pkill, через секунду снова должно быть 3 дочерних процесса."
pkill -f "$WORKDIR/proc2"
sleep 1
count="$(child_count "$daemon_pid")"
write_result "Фактически найдено процессов после рестарта: $count"
show_children "$daemon_pid" >> "$RESULT"
if [ "$count" -ne 3 ]; then
    write_result "Итог: FAIL"
    exit 1
fi
write_result "Итог: OK"
write_result ""

write_result "Тест 3: заменить конфиг на один процесс и отправить SIGHUP, должен остаться 1 дочерний процесс."
cp "$WORKDIR/config1.conf" "$WORKDIR/current.conf"
kill -HUP "$daemon_pid"
sleep 2
count="$(child_count "$daemon_pid")"
write_result "Фактически найдено процессов после SIGHUP: $count"
show_children "$daemon_pid" >> "$RESULT"
if [ "$count" -ne 1 ]; then
    write_result "Итог: FAIL"
    exit 1
fi
write_result "Итог: OK"
write_result ""

write_result "Проверка лога: ожидаются START трех процессов, EXIT и RESTART процесса 2, STOP трех процессов, START одного процесса."
write_result "--- /tmp/myinit.log ---"
cat "$LOG" >> "$RESULT"
write_result "--- конец лога ---"

starts_before_hup="$(awk '/SIGHUP received/ { exit } /START line=/ { count++ } END { print count + 0 }' "$LOG")"
restarts="$(grep -c 'RESTART line=2' "$LOG" || true)"
stops="$(grep -c 'STOP line=' "$LOG" || true)"
starts_total="$(grep -c 'START line=' "$LOG" || true)"

write_result "Фактически: START до SIGHUP=$starts_before_hup, RESTART line=2=$restarts, STOP=$stops, START всего=$starts_total"
if [ "$starts_before_hup" -ge 3 ] && [ "$restarts" -ge 1 ] && [ "$stops" -ge 3 ] && [ "$starts_total" -ge 5 ]; then
    write_result "Итог проверки лога: OK"
else
    write_result "Итог проверки лога: FAIL"
    exit 1
fi

write_result ""
write_result "Общий итог: OK"
