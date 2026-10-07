#!/usr/bin/env bash
# Probe the unofficial Google Translate endpoint before writing the C++ provider.
#
# Answers:
#   T1  does a basic GET work from this network?
#   T2  are datasheet terms (register names, part numbers, units) kept?
#   T3  can several paragraphs be batched in one request and split back?
#   T4  how large can one POST body be?
#   T5  (optional, -r) how many requests until we get throttled?
#
# Usage: ./google-translate-spike.sh [-t zh-TW] [-o outdir] [-r count] [-i interval_sec]
# Needs: bash, curl, jq. Runs on Linux, WSL or Git Bash on Windows.
# Every raw response is saved in outdir so it can become a parser test fixture.

set -u

# SPIKE_ENDPOINT overrides the URL, e.g. for a local mock server
ENDPOINT="${SPIKE_ENDPOINT:-https://translate.googleapis.com/translate_a/single}"
TARGET="zh-TW"
OUTDIR="./spike-out"
RATE_COUNT=0
RATE_INTERVAL=1

while getopts "t:o:r:i:h" opt; do
    case "$opt" in
    t) TARGET="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    r) RATE_COUNT="$OPTARG" ;;
    i) RATE_INTERVAL="$OPTARG" ;;
    *) sed -n '2,15p' "$0"; exit 1 ;;
    esac
done

# install hints:
#   Ubuntu / WSL:  sudo apt install curl jq
#   Git Bash:      winget install jqlang.jq   (then open a new Git Bash)
#   macOS:         brew install jq
for tool in curl jq; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool (see install hints at the top of $0)" >&2; exit 1; }
done
mkdir -p "$OUTDIR"
REPORT="$OUTDIR/report.txt"
: >"$REPORT"

log() { echo "$*" | tee -a "$REPORT"; }

# Send one request. GET puts q in the URL, POST puts it in the form body.
# Prints "<http_code> <seconds>" and writes the body to $3.
call() {
    local method="$1" text="$2" out="$3"
    local params=(--data-urlencode "client=gtx" --data-urlencode "sl=en"
        --data-urlencode "tl=$TARGET" --data-urlencode "dt=t"
        --data-urlencode "q=$text")
    if [ "$method" = "GET" ]; then
        curl -sS -G "${params[@]}" -o "$out" -w "%{http_code} %{time_total}" "$ENDPOINT"
    else
        curl -sS -X POST "${params[@]}" -o "$out" -w "%{http_code} %{time_total}" "$ENDPOINT"
    fi
}

# ok | blocked | error, based on status code and body.
# Google returns an HTML "Sorry..." page (often with 429) when it blocks an IP.
classify() {
    local code="$1" body="$2"
    if [ "$code" = "200" ] && jq -e . "$body" >/dev/null 2>&1; then
        echo ok
    elif [ "$code" = "429" ] || grep -q "Sorry" "$body" 2>/dev/null; then
        echo blocked
    else
        echo error
    fi
}

# Join the translated segments: response is [[["dst","src",...],...],...]
translated() { jq -r '[.[0][] | .[0] // ""] | join("")' "$1"; }

# --- T1 basic GET ---------------------------------------------------------
log "== T1 basic GET"
read -r code secs < <(call GET "The device enters sleep mode." "$OUTDIR/t1.json")
state=$(classify "$code" "$OUTDIR/t1.json")
log "http=$code time=${secs}s state=$state"
if [ "$state" != ok ]; then
    log "endpoint not usable from this network; stopping (see $OUTDIR/t1.json)"
    exit 2
fi
log "result: $(translated "$OUTDIR/t1.json")"

# --- T2 datasheet terms ---------------------------------------------------
log ""
log "== T2 datasheet terms"
T2_TEXT="Set bit 3 of GPIOA_MODER to 1 to enable the PLL. The STM32F407VG supports VDD from 1.8 V to 3.6 V and I2C up to 400 kHz."
read -r code secs < <(call GET "$T2_TEXT" "$OUTDIR/t2.json")
log "http=$code state=$(classify "$code" "$OUTDIR/t2.json")"
T2_OUT=$(translated "$OUTDIR/t2.json")
log "result: $T2_OUT"
for term in GPIOA_MODER PLL STM32F407VG VDD "1.8 V" "3.6 V" I2C "400 kHz"; do
    if [[ "$T2_OUT" == *"$term"* ]]; then log "  kept    $term"; else log "  CHANGED $term"; fi
done

# --- T3 batching ----------------------------------------------------------
# Paragraphs joined by a blank line; check the same number come back.
log ""
log "== T3 batching with blank-line delimiter"
PARAS=(
    "The watchdog timer resets the system if software stops responding."
    "Each DMA stream can be configured for memory-to-peripheral transfers."
    "Power consumption depends on the clock frequency and the enabled peripherals."
    "The bootloader is stored in system memory and is activated through the BOOT0 pin."
    "Refer to the reference manual for the full register description."
)
BATCH=$(printf '%s\n\n' "${PARAS[@]}")
BATCH=${BATCH%$'\n\n'}
read -r code secs < <(call POST "$BATCH" "$OUTDIR/t3.json")
log "http=$code time=${secs}s state=$(classify "$code" "$OUTDIR/t3.json")"
T3_OUT=$(translated "$OUTDIR/t3.json")
printf '%s\n' "$T3_OUT" >"$OUTDIR/t3.txt"
got=$(printf '%s' "$T3_OUT" | awk 'BEGIN{RS=""} END{print NR}')
log "paragraphs sent=${#PARAS[@]} received=$got"
if [ "$got" = "${#PARAS[@]}" ]; then log "  delimiter preserved"; else log "  DELIMITER LOST (see $OUTDIR/t3.txt)"; fi

# --- T4 POST size limit ---------------------------------------------------
log ""
log "== T4 POST body size"
SENTENCE="The peripheral clock must be enabled before any register is written. "
for size in 1000 5000 10000 20000 40000; do
    text=""
    while [ ${#text} -lt "$size" ]; do text+="$SENTENCE"; done
    text=${text:0:$size}
    read -r code secs < <(call POST "$text" "$OUTDIR/t4-$size.json")
    state=$(classify "$code" "$OUTDIR/t4-$size.json")
    log "chars=$size http=$code time=${secs}s state=$state"
    [ "$state" = ok ] || break
    sleep 1
done

# --- T5 rate probe (optional) ---------------------------------------------
if [ "$RATE_COUNT" -gt 0 ]; then
    log ""
    log "== T5 rate probe: $RATE_COUNT requests, ${RATE_INTERVAL}s apart"
    for i in $(seq 1 "$RATE_COUNT"); do
        read -r code secs < <(call GET "Request number $i of the rate probe." "$OUTDIR/t5.json")
        state=$(classify "$code" "$OUTDIR/t5.json")
        log "req=$i http=$code time=${secs}s state=$state"
        if [ "$state" != ok ]; then
            cp "$OUTDIR/t5.json" "$OUTDIR/t5-blocked.html"
            log "throttled after $((i - 1)) successful requests"
            break
        fi
        sleep "$RATE_INTERVAL"
    done
fi

log ""
log "report: $REPORT"
