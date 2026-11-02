#!/usr/bin/env bash
# Build everything and start the web demo at http://127.0.0.1:8080.
# Local use only: demo mode has no authentication.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -Wno-dev -DFPX_BUILD_TESTS=OFF
cmake --build build -j --target fpx_server
(cd apps/web && npm ci --no-audit --no-fund && npm run build)

DB="$(mktemp -d)/demo.fpxdb"
echo "Demo gallery: $DB (starts empty)"
echo "Open http://127.0.0.1:8080"
exec build/fpx-server --db "$DB" --host 127.0.0.1 --port 8080 --demo --web-root apps/web/dist
