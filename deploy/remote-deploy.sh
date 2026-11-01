#!/usr/bin/env bash
# Runs ON the deployment server (invoked over SSH by .github/workflows/deploy.yml).
#
#   remote-deploy.sh <image> [health-url]
#
# Pulls <image>, restarts the service, waits until /healthz answers and rolls
# back to the previously deployed image if it doesn't. Templates are kept in
# the named volume, so a restart only reloads them into RAM.
set -euo pipefail

IMAGE="${1:?usage: remote-deploy.sh <image> [health-url]}"
HEALTH_URL="${2:-http://127.0.0.1:8080/healthz}"
DIR="$(cd "$(dirname "$0")" && pwd)"
STATE="$DIR/.deploy-state"
# Loading 1M templates takes ~15-30 s; allow generous start-up time.
TIMEOUT="${FPX_HEALTH_TIMEOUT:-300}"

cd "$DIR"
[ -f .env ] || { echo "missing $DIR/.env (must contain FPX_API_KEY=...)" >&2; exit 1; }

compose() { FPX_IMAGE="$1" docker compose -f docker-compose.yml "${@:2}"; }

healthy() {
  local deadline=$((SECONDS + TIMEOUT))
  while [ "$SECONDS" -lt "$deadline" ]; do
    if curl -fsS --max-time 3 "$HEALTH_URL" 2>/dev/null | grep -q '"ok"'; then return 0; fi
    sleep 3
  done
  return 1
}

PREVIOUS=""
[ -f "$STATE" ] && PREVIOUS="$(sed -n 's/^current=//p' "$STATE")"

echo "deploying $IMAGE (previous: ${PREVIOUS:-none})"
compose "$IMAGE" pull
compose "$IMAGE" up -d --remove-orphans

if healthy; then
  printf 'current=%s\nprevious=%s\ndeployed_at=%s\n' "$IMAGE" "$PREVIOUS" "$(date -u +%FT%TZ)" > "$STATE"
  echo "deploy OK: $IMAGE"
  docker image prune -f >/dev/null 2>&1 || true
  exit 0
fi

echo "health check failed for $IMAGE" >&2
compose "$IMAGE" logs --tail 100 >&2 || true
if [ -n "$PREVIOUS" ] && [ "$PREVIOUS" != "$IMAGE" ]; then
  echo "rolling back to $PREVIOUS" >&2
  compose "$PREVIOUS" up -d --remove-orphans
  if healthy; then echo "rollback OK" >&2; else echo "ROLLBACK ALSO UNHEALTHY - manual action needed" >&2; fi
fi
exit 1
