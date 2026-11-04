#!/usr/bin/env bash
# Build everything and run every test, benchmark, example and visual check.
#
#   scripts/run-all.sh            # default: everything except sanitizers and 1 M benchmarks (~15 min)
#   scripts/run-all.sh --quick    # fast loop: no Docker, smaller benchmarks and load test (~5 min)
#   scripts/run-all.sh --full     # also ASan/UBSan and the 1 M benchmarks (~30 min, ~4 GB RAM)
#   scripts/run-all.sh --only web,examples     # run selected stages
#   scripts/run-all.sh --skip docker           # skip selected stages
#
# Stages: tools build unit sanitizers accuracy bench examples gateway load web docker deploy
# Logs and a summary go to test-reports/<timestamp>/. Exit code 1 if any stage failed.
# Uses synthetic fingerprints only.
# shellcheck disable=SC2329  # stage functions are called by name through `stage`
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

MODE=default
ONLY=""
SKIP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) MODE=quick ;;
    --full) MODE=full ;;
    --only) ONLY=",$2,"; shift ;;
    --skip) SKIP=",$2,"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done

REPORT="$ROOT/test-reports/$(date +%Y%m%d-%H%M%S)"
WORK="$REPORT/work"
mkdir -p "$WORK"
BUILD="$ROOT/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
PORT_BASE="${FPX_TEST_PORT_BASE:-18700}"   # local ports used: PORT_BASE .. PORT_BASE+20
export PATH="$HOME/.local/bin:$PATH"
# Extra CMake arguments, e.g. pre-fetched dependencies in the test image
# (docker/Dockerfile.test). Word-split on purpose.
read -r -a CMAKE_EXTRA <<<"${FPX_CMAKE_ARGS:-}"
# Checkouts on drives owned by another user (NTFS/exFAT mounts, containers) make
# git refuse to work ("dubious ownership"). Trust them for this script only.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'

if [ -t 1 ]; then G=$'\e[32m'; R=$'\e[31m'; Y=$'\e[33m'; B=$'\e[1m'; N=$'\e[0m'; else G=; R=; Y=; B=; N=; fi
RESULTS=()
FAILED=0
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

want() {  # want <stage> [modes...]: selected by --only/--skip and by mode
  local s=$1; shift
  if [ -n "$ONLY" ]; then [[ "$ONLY" == *",$s,"* ]]; return; fi
  [[ "$SKIP" == *",$s,"* ]] && return 1
  [ $# -eq 0 ] && return 0
  local m; for m in "$@"; do [ "$m" = "$MODE" ] && return 0; done
  return 1
}

stage() {  # stage <name> <description> <function>
  local name=$1 desc=$2 fn=$3 log="$REPORT/$1.log" t0=$SECONDS
  printf '%s==> %-11s%s %s\n' "$B" "$name" "$N" "$desc"
  # A plain statement on purpose: inside `if`, `||` or `&&` bash ignores set -e
  # and a failing command would not fail the stage. Each stage stops its own servers.
  ( trap cleanup EXIT; set -e; "$fn" ) >"$log" 2>&1
  local rc=$?
  if [ "$rc" = 0 ]; then
    RESULTS+=("PASS|$name|$((SECONDS - t0))|$desc")
    printf '    %sPASS%s (%ss)\n' "$G" "$N" "$((SECONDS - t0))"
  else
    RESULTS+=("FAIL|$name|$((SECONDS - t0))|$desc")
    FAILED=1
    printf '    %sFAIL%s (%ss), last lines of %s:\n' "$R" "$N" "$((SECONDS - t0))" "${log#"$ROOT"/}"
    tail -15 "$log" | sed 's/^/      /'
  fi
}
skip() { RESULTS+=("SKIP|$1|0|$2"); printf '%s==> %-11s%s %sSKIP%s %s\n' "$B" "$1" "$N" "$Y" "$N" "$2"; }

have() { command -v "$1" >/dev/null 2>&1; }

start_server() {  # start_server <port> <args...>: background fpx-server, waits for /healthz
  local port=$1; shift
  "$BUILD/fpx-server" --host 127.0.0.1 --port "$port" "$@" >"$WORK/server-$port.log" 2>&1 &
  PIDS+=($!)
  wait_http "http://127.0.0.1:$port/healthz" 600
}
wait_http() {  # wait_http <url> <seconds>
  local i; for ((i = 0; i < $2 * 5; i++)); do curl -fs "$1" >/dev/null 2>&1 && return 0; sleep 0.2; done
  echo "timed out waiting for $1"; return 1
}
stop_all() { cleanup; PIDS=(); }

# --------------------------------------------------------------------------- stages
s_tools() {
  if ! have cmake; then
    echo "cmake not found, installing it for this user"
    if have uv; then uv tool install cmake; elif have pipx; then pipx install cmake; else python3 -m pip install --user cmake; fi
  fi
  if ! have shellcheck && have uv; then uv tool install shellcheck-py || true; fi
  for t in cmake g++ python3 curl; do have "$t" || { echo "missing required tool: $t (Ubuntu: sudo apt install build-essential cmake python3 curl)"; exit 1; }; done
  for t in cmake g++ python3 node npm go docker shellcheck; do
    printf '%-10s ' "$t"; if have "$t"; then "$t" --version 2>/dev/null | head -1 || "$t" version | head -1; else echo "not installed (stages that need it are skipped)"; fi
  done
}

s_build() {
  cmake -S . -B "$BUILD" -Wno-dev -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}"
  cmake --build "$BUILD" -j"$JOBS" 2>&1 | tee "$WORK/build-output.txt"
  if grep -E "warning:" "$WORK/build-output.txt" | grep -v "_deps/" ; then echo "compiler warnings in project code"; exit 1; fi
}

s_unit() { (cd "$BUILD" && ctest --output-on-failure --no-tests=error); }  # 0 tests must not pass

s_sanitizers() {
  local SB="$ROOT/build-asan" F="-fsanitize=address,undefined -fno-omit-frame-pointer"
  cmake -S . -B "$SB" -Wno-dev -DCMAKE_BUILD_TYPE=Debug -DFPX_BUILD_EXAMPLES=OFF \
    "-DCMAKE_CXX_FLAGS=$F" "-DCMAKE_C_FLAGS=$F" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined" -DFETCHCONTENT_BASE_DIR="$BUILD/_deps" "${CMAKE_EXTRA[@]}"
  cmake --build "$SB" -j"$JOBS"
  mkdir -p "$WORK/san"
  # log_path also catches reports from the servers that the e2e tests start
  (cd "$SB" && ASAN_OPTIONS="log_path=$WORK/san/asan" UBSAN_OPTIONS="log_path=$WORK/san/ubsan:print_stacktrace=1:suppressions=$ROOT/tests/ubsan.supp" \
     ctest --output-on-failure --no-tests=error)
  if ls "$WORK"/san/* >/dev/null 2>&1; then cat "$WORK"/san/*; echo "sanitizer reports found"; exit 1; fi
  echo "no ASan/UBSan reports"
}

s_accuracy() {
  local spec=50x4; [ "$MODE" = quick ] && spec=20x3
  "$BUILD/fpx" eval --synthetic "$spec" | tee "$WORK/eval.txt"
  grep -q "at default threshold 12.0: .*FMR 0.000 %" "$WORK/eval.txt"   # no false match
}

s_bench() {
  local runs=("--gallery 100000 --probes 20" "--gallery 100000 --probes 10 --fingers 2")
  [ "$MODE" = quick ] && runs=("--gallery 50000 --probes 10" "--gallery 20000 --probes 10 --fingers 2")
  [ "$MODE" = full ] && runs+=("--gallery 1000000 --probes 20" "--gallery 1000000 --probes 10 --fingers 2")
  local r; for r in "${runs[@]}"; do
    echo "### fpx bench $r"
    # shellcheck disable=SC2086
    "$BUILD/fpx" bench $r | tee "$WORK/bench.txt"
    if grep -q "FALSE ALARM" "$WORK/bench.txt"; then exit 1; fi
    if grep -q "false alarms" "$WORK/bench.txt"; then grep -q " 0 false alarms" "$WORK/bench.txt"; fi
  done
}

s_examples() {
  local D="$WORK/demo" T="$WORK/cli" P=$((PORT_BASE + 1))
  mkdir -p "$T"
  echo "### fpx demo";               "$BUILD/fpx" demo --out "$D"
  echo "### C++ example (synthetic)"; "$BUILD/example_pipeline"
  echo "### C++ example (files)";     "$BUILD/example_pipeline" "$D/finger1_0.bmp" "$D/finger1_1.bmp"
  echo "### Python ctypes";           python3 examples/python/fpx_ctypes.py "$BUILD/libfpx.so" "$D/finger1_0.bmp" "$D/finger1_1.bmp"

  echo "### CLI walk-through"
  "$BUILD/fpx" info "$D/finger1_0.bmp"
  "$BUILD/fpx" convert "$D/finger1_0.bmp" "$T/scan.wsq"
  "$BUILD/fpx" extract "$T/scan.wsq" -o "$T/scan.ansi" --format ansi
  "$BUILD/fpx" extract "$D/finger1_0.bmp" -o "$T/scan.iso" --format iso
  "$BUILD/fpx" totemplate "$T/scan.ansi" "$T/conv.iso" --format iso
  "$BUILD/fpx" info "$T/scan.ansi" -v | head -8
  "$BUILD/fpx" match "$T/scan.ansi" "$D/finger1_1.bmp" -v | tee "$T/m1"; grep -q "decision:  MATCH" "$T/m1"
  # exit code 1 means NO MATCH
  if "$BUILD/fpx" match "$T/scan.iso" "$D/finger2_1.bmp"; then echo "different fingers matched"; exit 1; fi
  "$BUILD/fpx" enroll --db "$T/t.fpxdb" --id alice "$D/finger1_0.bmp" "$D/finger2_0.ansi" --fingers 2,7
  "$BUILD/fpx" enroll --db "$T/t.fpxdb" --id bob "$D/finger3_0.iso" --fingers 2
  "$BUILD/fpx" identify --db "$T/t.fpxdb" "$D/finger1_1.bmp" "$D/finger2_1.iso" --fingers 2,7 | tee "$T/i1"
  grep -q "result: MATCH alice" "$T/i1"
  "$BUILD/fpx" identify --db "$T/t.fpxdb" "$D/finger3_1.bmp" --fingers 2 | tee "$T/i2"
  grep -q "result: MATCH bob" "$T/i2"
  if "$BUILD/fpx" identify --db "$T/t.fpxdb" "$D/finger3_1.bmp" --fingers 7; then echo "expected no match"; exit 1; fi
  "$BUILD/fpx" remove --db "$T/t.fpxdb" --id bob
  "$BUILD/fpx" compact --db "$T/t.fpxdb"

  echo "### Python REST client"
  start_server "$P" --db "$T/rest.fpxdb" --api-key example-key
  export FPX_URL="http://127.0.0.1:$P" FPX_API_KEY=example-key
  python3 examples/python/client.py enroll alice "$D/finger1_0.iso:2" "$D/finger2_0.bmp:7"
  python3 examples/python/client.py identify "$D/finger1_1.bmp:2" "$D/finger2_1.iso:7" | tee "$T/r1"
  grep -q '"id": "alice"' "$T/r1"
  python3 examples/python/client.py verify alice "$D/finger3_1.iso:2" | tee "$T/r2"
  grep -q '"match": false' "$T/r2"
  python3 examples/python/client.py get alice
  python3 examples/python/client.py stats
  echo "### examples/http/requests.http routes exist on the server"
  # an unknown route is a 404 with an empty body; an unknown id is a 404 with a JSON error
  local m path out; while read -r m path; do
    path=${path#\{\{host\}\}}; path=${path//customer-42/runall-nobody}
    out=$(curl -s -w ' %{http_code}' -X "$m" -H "X-API-Key: example-key" "http://127.0.0.1:$P$path" -d '{}')
    echo "$m $path -> $out"
    [ "$out" != " 404" ] || { echo "route missing on the server"; exit 1; }
  done < <(grep -E '^(GET|POST|PUT|DELETE) ' examples/http/requests.http)
  stop_all
}

s_gateway() {
  have go || { echo "go not installed"; exit 1; }
  local D="$WORK/demo" GW="$WORK/gateway"
  [ -d "$D" ] || "$BUILD/fpx" demo --out "$D" >/dev/null
  (cd examples/go-gateway && go vet ./... && go build -o "$GW" .)
  start_server $((PORT_BASE + 2)) --db "$WORK/shard0.fpxdb" --api-key shard-key
  start_server $((PORT_BASE + 3)) --db "$WORK/shard1.fpxdb" --api-key shard-key
  "$GW" -shards "http://127.0.0.1:$((PORT_BASE + 2)),http://127.0.0.1:$((PORT_BASE + 3))" \
    -listen "127.0.0.1:$((PORT_BASE + 4))" -shard-key shard-key -api-key client-key >"$WORK/gateway.log" 2>&1 &
  PIDS+=($!)
  local i; for ((i = 0; i < 50; i++)); do curl -s -o /dev/null "http://127.0.0.1:$((PORT_BASE + 4))/" && break; sleep 0.2; done
  python3 tests/gateway_e2e.py --gateway "http://127.0.0.1:$((PORT_BASE + 4))" --demo "$D" --api-key client-key \
    --shards "http://127.0.0.1:$((PORT_BASE + 2)),http://127.0.0.1:$((PORT_BASE + 3))" --shard-key shard-key
  stop_all
}

s_load() {
  local n=100000 dur=15 conc=1,4,16 P=$((PORT_BASE + 5))
  [ "$MODE" = quick ] && { n=10000; dur=8; conc=1,8; }
  "$BUILD/fpx" gendb --db "$WORK/load.fpxdb" --subjects "$n" --fingers 2 --probes 40 --probe-dir "$WORK/load-probes"
  start_server "$P" --db "$WORK/load.fpxdb" --api-key load-key
  python3 tests/load_test.py --url "http://127.0.0.1:$P" --api-key load-key --probes "$WORK/load-probes" \
    --concurrency "$conc" --duration "$dur" --json "$REPORT/load.json"
  stop_all
}

s_web() {
  have npm || { echo "npm not installed"; exit 1; }
  cd apps/web
  npm ci --no-audit --no-fund --prefer-offline
  npm test
  npm run build
  npx playwright install chromium
  # Visual checks compare against e2e/snapshots/*-<platform>.png. If this OS has no
  # baselines yet, they are written on this first run: review them, then commit.
  FPX_SERVER="$BUILD/fpx-server" npx playwright test --reporter=line
  mkdir -p "$REPORT/screenshots"
  cp e2e/snapshots/*-"$(node -p process.platform)".png "$REPORT/screenshots/" 2>/dev/null || true
}

s_docker() {
  if ! have docker || ! docker info >/dev/null 2>&1; then echo "docker not available"; exit 1; fi
  local P=$((PORT_BASE + 6)) name="fpx-runall-$$"
  docker build -f docker/Dockerfile -t fpx:runall .
  docker run -d --name "$name" -p "127.0.0.1:$P:8080" -e FPX_API_KEY=ci fpx:runall
  # Expanded now: the trap runs after this function's locals are gone (set -u).
  # shellcheck disable=SC2064
  trap "docker rm -f $name >/dev/null 2>&1" EXIT
  wait_http "http://127.0.0.1:$P/healthz" 120
  curl -fsS -H 'X-API-Key: ci' "http://127.0.0.1:$P/v1/stats"; echo
  [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$P/v1/stats")" = 401 ]
  local i s; for ((i = 0; i < 40; i++)); do
    s=$(docker inspect -f '{{.State.Health.Status}}' "$name"); echo "health: $s"
    [ "$s" = healthy ] && break; sleep 5
  done
  [ "$s" = healthy ]

  echo "### docker compose: 2 shards + Go gateway"
  local D="$WORK/demo" proj="fpxrunall$$"
  [ -d "$D" ] || "$BUILD/fpx" demo --out "$D" >/dev/null
  # shellcheck disable=SC2064
  trap "docker rm -f $name >/dev/null 2>&1; docker compose -p $proj -f docker/docker-compose.yml --profile sharded down -v >/dev/null 2>&1" EXIT
  docker compose -p "$proj" -f docker/docker-compose.yml --profile sharded up -d --build shard0 shard1 gateway
  # the gateway container compiles itself with `go run` on first start
  local ok=0; for ((i = 0; i < 120; i++)); do curl -s -o /dev/null "http://127.0.0.1:9090/" && { ok=1; break; }; sleep 2; done
  [ "$ok" = 1 ] || { docker compose -p "$proj" -f docker/docker-compose.yml --profile sharded logs; exit 1; }
  python3 tests/gateway_e2e.py --gateway http://127.0.0.1:9090 --demo "$D"
}

s_deploy() {
  if have shellcheck; then shellcheck deploy/remote-deploy.sh scripts/*.sh; else echo "shellcheck not installed, skipped"; fi
  if have docker; then
    local made=0; [ -e deploy/.env ] || { touch deploy/.env; made=1; }
    (cd deploy && FPX_IMAGE=example/image:tag docker compose -f docker-compose.prod.yml config -q); local rc=$?
    [ "$made" = 1 ] && rm -f deploy/.env
    [ "$rc" = 0 ]
    docker compose -f docker/docker-compose.yml --profile sharded config -q
  fi
  echo "deploy files valid"
}

# --------------------------------------------------------------------------- run
echo "${B}fp-extractor: run everything (mode: $MODE)${N}  report: ${REPORT#"$ROOT"/}"
want tools && stage tools "toolchain check (installs cmake if missing)" s_tools
want build && stage build "Release build, no warnings in project code" s_build
want unit && stage unit "unit + integration + CLI + REST e2e tests (ctest)" s_unit
if want sanitizers full; then stage sanitizers "Debug build with ASan/UBSan, all tests" s_sanitizers; elif want sanitizers; then skip sanitizers "(--full)"; fi
want accuracy && stage accuracy "synthetic accuracy (EER, FNMR, FMR)" s_accuracy
want bench && stage bench "1:N benchmarks (1 and 2 fingers$([ "$MODE" = full ] && echo ', 1 M'))" s_bench
want examples && stage examples "C++, Python ctypes, CLI, REST client, requests.http" s_examples
if want gateway && have go; then stage gateway "Go gateway in front of 2 shards" s_gateway; elif want gateway; then skip gateway "(go not installed)"; fi
want load && stage load "HTTP load test: throughput, latency, consistency" s_load
if want web && have npm; then stage web "web unit tests, build, Playwright visual e2e" s_web; elif want web; then skip web "(npm not installed)"; fi
if want docker default full && have docker; then stage docker "Docker image, HEALTHCHECK, compose sharded" s_docker; elif want docker; then skip docker "$([ "$MODE" = quick ] && echo '(not in --quick)' || echo '(docker not available)')"; fi
want deploy && stage deploy "shellcheck + compose file validation" s_deploy

{
  echo "# run-all report ($(date '+%Y-%m-%d %H:%M'), mode $MODE)"
  echo
  echo "| Result | Stage | Seconds | What |"
  echo "|---|---|---|---|"
  for r in "${RESULTS[@]}"; do IFS='|' read -r res name secs desc <<<"$r"; echo "| $res | $name | $secs | $desc |"; done
  echo
  echo "Logs: one \`<stage>.log\` per stage in this folder."
} >"$REPORT/summary.md"

echo
echo "${B}Summary${N} (${REPORT#"$ROOT"/}/summary.md)"
for r in "${RESULTS[@]}"; do
  IFS='|' read -r res name secs desc <<<"$r"
  c=$G; [ "$res" = FAIL ] && c=$R; [ "$res" = SKIP ] && c=$Y
  printf '  %s%-4s%s  %-11s %5ss  %s\n' "$c" "$res" "$N" "$name" "$secs" "$desc"
done
[ $FAILED = 0 ] && echo "${G}${B}ALL PASSED${N}" || echo "${R}${B}SOME STAGES FAILED${N}"
exit $FAILED
