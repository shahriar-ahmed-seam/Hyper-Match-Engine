#!/usr/bin/env bash
# Build (if needed) and launch the full stack: the C++ matching engine,
# the Rust gateway, and the web console.
#
# Usage:  ./scripts/run.sh [engine_port] [gateway_port]
set -euo pipefail

cd "$(dirname "$0")/.."

ENGINE_PORT="${1:-9001}"
GATEWAY_PORT="${2:-8080}"

echo "==> Building matching engine (C++)"
[ -d cpp/build ] || cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build

echo "==> Building gateway (Rust)"
cargo build --release -p gateway-server

echo "==> Starting engine on 127.0.0.1:${ENGINE_PORT}"
./cpp/build/server/hme_engine_server --host 127.0.0.1 --port "${ENGINE_PORT}" &
ENGINE_PID=$!
sleep 1

echo "==> Starting gateway on 127.0.0.1:${GATEWAY_PORT}"
./target/release/hme-gateway \
    --listen "127.0.0.1:${GATEWAY_PORT}" \
    --engine "127.0.0.1:${ENGINE_PORT}" \
    --web ./web &
GATEWAY_PID=$!

cleanup() { kill "${GATEWAY_PID}" "${ENGINE_PID}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

echo ""
echo "Console:  http://localhost:${GATEWAY_PORT}   (Ctrl+C to stop)"
wait
