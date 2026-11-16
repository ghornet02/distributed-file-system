#!/usr/bin/env bash
set -u

cd "$(dirname "$0")/.."
TMP=tests/tmp
rm -rf "$TMP"
mkdir -p "$TMP"

NM_PORT=$((20000 + RANDOM % 20000))
SS1_PORT=$((NM_PORT + 1))
SS2_PORT=$((NM_PORT + 2))
PIDS=()
PASS=0
FAIL=0

cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT

start_ss() {
    ./storageserver "$1" "$TMP/ss$2" 127.0.0.1 "$NM_PORT" >"$TMP/ss$2.log" 2>&1 &
    PIDS+=($!)
    eval "SS$2_PID=$!"
}

client() {
    local user=$1
    shift
    printf '%s\n' "$user" "$@" EXIT | ./client 127.0.0.1 "$NM_PORT" 2>&1
}

check() {
    local desc=$1 pattern=$2 output=$3
    if grep -qF -- "$pattern" <<<"$output"; then
        PASS=$((PASS + 1))
        echo "  ok   $desc"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL $desc (expected: $pattern)"
        sed 's/^/       | /' <<<"$output"
    fi
}

wait_for() {
    local pattern=$1 file=$2
    for _ in $(seq 1 50); do
        grep -qF -- "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

./nameserver "$NM_PORT" 3 >"$TMP/nm.log" 2>&1 &
PIDS+=($!)
sleep 0.3
start_ss "$SS1_PORT" 1
start_ss "$SS2_PORT" 2
wait_for "127.0.0.1:$SS1_PORT online" "$TMP/nm.log"
wait_for "127.0.0.1:$SS2_PORT online" "$TMP/nm.log"

echo "Registration and file creation"
out=$(client alice "CREATE notes.txt" "CREATE todo.txt" "CREATE notes.txt" "CREATE ../evil")
check "create first file" "OK File created on" "$out"
check "duplicate name rejected" "ERROR File already exists" "$out"
check "path traversal rejected" "ERROR Invalid file name" "$out"
spread=$(grep -o 'on 127.0.0.1:[0-9]*' <<<"$out" | sort -u | wc -l | tr -d ' ')
check "files spread across both storage servers" "2" "$spread"

echo "Writing and reading"
out=$(client alice "WRITE notes.txt hello world" "WRITE notes.txt .starts with a dot" "WRITE notes.txt STOP" "WRITE notes.txt last line" "READ notes.txt")
check "write acknowledged" "OK Wrote 12 bytes" "$out"
check "read returns content" "hello world" "$out"
check "leading dot preserved" ".starts with a dot" "$out"
check "content line STOP does not end the stream" "last line" "$out"

echo "Listing and metadata"
out=$(client bob "VIEW" "VIEW -a" "INFO notes.txt" "INFO missing.txt")
check "VIEW lists files" "notes.txt (owner=alice)" "$out"
check "VIEW -a shows storage server" "[online]" "$out"
check "INFO reports owner" "owner=alice" "$out"
check "INFO on missing file" "ERROR File not found" "$out"

echo "Permissions"
out=$(client bob "READ notes.txt" "WRITE notes.txt hacked" "DELETE notes.txt")
check "other users can read" "hello world" "$out"
check "other users cannot write" "ERROR Permission denied: only the owner can write" "$out"
check "other users cannot delete" "ERROR Permission denied: only the owner can delete" "$out"

echo "Deletion"
out=$(client alice "DELETE todo.txt" "READ todo.txt" "VIEW")
check "owner can delete" "OK File deleted" "$out"
check "deleted file is gone" "ERROR File not found" "$out"
if [ -e "$TMP/ss1/todo.txt" ] || [ -e "$TMP/ss2/todo.txt" ]; then
    FAIL=$((FAIL + 1)); echo "  FAIL file removed from storage server disk"
else
    PASS=$((PASS + 1)); echo "  ok   file removed from storage server disk"
fi

echo "Storage server failure and recovery"
holder=$(grep -o 'notes.txt on 127.0.0.1:[0-9]*' "$TMP/nm.log" | grep -o '[0-9]*$')
if [ "$holder" = "$SS1_PORT" ]; then victim=1; vport=$SS1_PORT; else victim=2; vport=$SS2_PORT; fi
eval "kill \$SS${victim}_PID"
wait_for "127.0.0.1:$vport timed out" "$TMP/nm.log"
out=$(client alice "READ notes.txt" "INFO notes.txt")
check "read fails while server is down" "ERROR Storage server offline" "$out"
check "INFO reports offline" "status=offline" "$out"
start_ss "$vport" "$victim"
for _ in $(seq 1 50); do
    out=$(client alice "READ notes.txt")
    grep -qF "hello world" <<<"$out" && break
    sleep 0.2
done
check "file readable again after restart" "hello world" "$out"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
