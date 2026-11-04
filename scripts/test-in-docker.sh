#!/usr/bin/env bash
# Build the test image (docker/Dockerfile.test) and run scripts/run-all.sh in it.
# Needs only Docker on the host. Arguments go to run-all.sh; default: everything
# except the Docker stage, which needs a daemon (run it on the host with
# scripts/run-all.sh --only docker).
#
#   scripts/test-in-docker.sh                     # default run
#   scripts/test-in-docker.sh --quick --skip docker
#   scripts/test-in-docker.sh --full --skip docker
#   scripts/test-in-docker.sh --update-snapshots  # rewrite the Playwright baselines from the image
#
# Reports are written to test-reports/ on the host.
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE="${FPX_TEST_IMAGE:-fp-extractor-test}"

if [ "${1:-}" = "--update-snapshots" ]; then
  shift
  UPDATE=1
else
  UPDATE=0
fi

docker build -f docker/Dockerfile.test -t "$IMAGE" .
mkdir -p test-reports
# Chromium needs more shared memory than Docker's 64 MB default.
RUN=(docker run --rm --init --shm-size=1g --user "$(id -u):$(id -g)" -v "$PWD/test-reports:/src/test-reports")

if [ "$UPDATE" = 1 ]; then
  # The image is the reference renderer: write baselines there and copy them back.
  # shellcheck disable=SC2016  # expanded inside the container
  "${RUN[@]}" -v "$PWD/apps/web/e2e/snapshots:/src/apps/web/e2e/snapshots" --entrypoint bash "$IMAGE" -c '
    set -e
    cmake -S . -B build -Wno-dev $FPX_CMAKE_ARGS >/dev/null && cmake --build build -j"$(nproc)" --target fpx_server >/dev/null
    cd apps/web && npm run build >/dev/null
    FPX_SERVER=/src/build/fpx-server npx playwright test --reporter=line --update-snapshots=all'
  echo "updated apps/web/e2e/snapshots/*-linux.png; review and commit them"
  exit 0
fi

[ $# -eq 0 ] && set -- --skip docker
exec "${RUN[@]}" "$IMAGE" "$@"
