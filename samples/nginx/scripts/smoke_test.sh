#!/usr/bin/env bash
# Smoke test for the nginx sample.
#
# The script sends HTTP requests to the running instance and checks:
#   1. GET / returns HTTP 200 and the test page.
#   2. A second GET / succeeds, so the server accepts a new connection.
#   3. GET /missing-page returns HTTP 404.
#
# Usage: ./scripts/smoke_test.sh <host> [attempts] [delay_seconds]
# Exit code: 0 when all checks pass, 1 otherwise.
set -uo pipefail

# Ignore proxy settings so requests go directly to the target host.
export no_proxy="*"
export NO_PROXY="*"

HOST="${1:-}"
ATTEMPTS="${2:-10}"
DELAY="${3:-5}"
MARKER="Unikraft nginx on AWS EC2"
TIMEOUT=10

if [ -z "${HOST}" ]; then
    echo "[ERR] Usage: $0 <host> [attempts] [delay_seconds]"
    exit 1
fi

TMP_BODY="$(mktemp)"
trap 'rm -f "${TMP_BODY}"' EXIT

# Waits until the host answers HTTP at all. The unikernel needs a few
# seconds to boot, get an address, and start nginx.
wait_for_http() {
    local attempt
    for attempt in $(seq 1 "${ATTEMPTS}"); do
        local code
        code="$(curl -sS -m "${TIMEOUT}" -o "${TMP_BODY}" -w '%{http_code}' "http://${HOST}/" 2>/dev/null || true)"
        if [ "${code}" != "000" ] && [ -n "${code}" ]; then
            echo "[INFO] Host ${HOST} answered with HTTP ${code} on attempt ${attempt}."
            return 0
        fi
        echo "[INFO] Attempt ${attempt}/${ATTEMPTS}: no HTTP answer yet. Waiting ${DELAY}s..."
        sleep "${DELAY}"
    done
    return 1
}

check_status() {
    local path="$1" want="$2" label="$3"
    local code
    code="$(curl -sS -m "${TIMEOUT}" -o "${TMP_BODY}" -w '%{http_code}' "http://${HOST}${path}")"
    if [ "${code}" = "${want}" ]; then
        echo "[OK]   ${label}: HTTP ${code}"
        return 0
    fi
    echo "[FAIL] ${label}: expected HTTP ${want}, got ${code}"
    return 1
}

echo "[INFO] Waiting for HTTP on ${HOST} ..."
if ! wait_for_http; then
    echo "[FAIL] ${HOST} did not answer HTTP after ${ATTEMPTS} attempts."
    exit 1
fi

FAILED=0

if ! check_status "/" "200" "GET /"; then
    FAILED=1
fi

if grep -q "${MARKER}" "${TMP_BODY}"; then
    echo "[OK]   Response body contains the test page marker."
else
    echo "[FAIL] Response body does not contain '${MARKER}'."
    FAILED=1
fi

if ! check_status "/" "200" "GET / (second connection)"; then
    FAILED=1
fi

if ! check_status "/missing-page" "404" "GET /missing-page"; then
    FAILED=1
fi

if [ "${FAILED}" -eq 0 ]; then
    echo "[SUCCESS] Smoke test passed for ${HOST}."
else
    echo "[FAIL] Smoke test failed for ${HOST}."
fi
exit "${FAILED}"
