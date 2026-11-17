#!/usr/bin/env bash
#
# TFB compliance validator for the httpreply-mc endpoints.
#
# Checks a running HTTP server against the TechEmpower-style rules
# the Plan-TFB-Phase1 wiki page records:
#   1. /plaintext and /json body bytes, exact Content-Type, and
#      Content-Length equal to the body length. The expected
#      literals are the bare text/plain and application/json. The
#      official TFB verifier (toolset/test_types/verifications.py,
#      verify_headers, checked 2026-11-18) makes the charset suffix
#      optional, so the bare form passes its regex. [Ticket
#      21214f6a58]
#   2. Server and Date headers present. Date parses as RFC 1123 and
#      sits within one minute of now.
#   3. The /json key case is exactly "message".
#   4. Keep-alive works on one connection.
#   5. HTTP/1.1 pipelining: 100 GETs sent at once return 100
#      well-formed responses in order.
#   6. Unknown paths answer 404.
#
# The unikernel needs the ENA device, which QEMU cannot model. Use
# the host harness for local runs:
#
#   make -C ../../.. build/test_tfb_http
#   ../../../build/test_tfb_http --serve 8080 &
#   ./tfb_validate.sh 127.0.0.1 8080
#
# Point it at a live target the same way, for example an EC2 host:
#
#   ./tfb_validate.sh <host> <port>
#
# Set CHECK_LEGACY=1 to also require the legacy "/" reply the wrk
# harness depends on. Run it only against httpreply-mc, not against
# a nginx baseline.
#
# Exits non-zero on any failure.
#
# [Ticket c3dd68d3d3]
set -u

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"

if ! command -v python3 >/dev/null 2>&1; then
	echo "[ERR] python3 is required" >&2
	exit 2
fi

HOST="$HOST" PORT="$PORT" CHECK_LEGACY="${CHECK_LEGACY:-0}" python3 - <<'PYEOF'
import os
import re
import socket
import sys
import time
from email.utils import parsedate_to_datetime

HOST = os.environ["HOST"]
PORT = int(os.environ["PORT"])
CHECK_LEGACY = os.environ.get("CHECK_LEGACY") == "1"

PLAIN_BODY = b"Hello, World!"
JSON_BODY = b'{"message":"Hello, World!"}'
TIMEOUT = 10.0
RFC1123 = re.compile(
    r"^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d{2} "
    r"(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} "
    r"\d{2}:\d{2}:\d{2} GMT$"
)

failures = 0


def report(ok, label, detail=""):
    global failures
    if not ok:
        failures += 1
    print(("[PASS] " if ok else "[FAIL] ") + label +
          ((" " + detail) if detail else ""))


def connect():
    s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    s.settimeout(TIMEOUT)
    return s


def parse_one(data):
    """Split one response from the head of data.

    Returns (status, headers, body, rest) or None when data does not
    yet hold one full response, or ("ERR", reason) on a fault.
    """
    sep = data.find(b"\r\n\r\n")
    if sep < 0:
        return None
    head = data[:sep].decode("latin-1")
    rest = data[sep + 4:]
    lines = head.split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) < 2:
        return ("ERR", "malformed status line: %r" % lines[0])
    status = parts[1]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    if "Content-Length" in headers or "content-length" in headers:
        clen = int(headers["content-length"])
        if len(rest) < clen:
            return None
        body = rest[:clen]
        return (status, headers, body, rest[clen:])
    if headers.get("transfer-encoding", "").lower() == "chunked":
        return ("ERR", "chunked encoding is not allowed (no gzip/stream "
                       "trickery expected here)")
    return ("ERR", "missing Content-Length")


def read_response(sock, buf=b""):
    while True:
        got = parse_one(buf)
        if isinstance(got, tuple) and got and got[0] == "ERR":
            return got, buf
        if got is not None:
            return got, got[3]
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            return ("ERR", "timed out after %.0fs waiting for response" %
                    TIMEOUT), buf
        except OSError as exc:
            return ("ERR", "recv error: %s" % exc), buf
        if not chunk:
            return ("ERR", "connection closed mid-response"), buf
        buf += chunk


def check_headers(label, headers, expect_ct, expect_body):
    ok = True
    ct = headers.get("content-type", "")
    if ct != expect_ct:
        ok = False
        report(False, label + " Content-Type", "got %r want %r" %
               (ct, expect_ct))
    else:
        report(True, label + " Content-Type: " + ct)

    cl = headers.get("content-length")
    if cl is None:
        ok = False
        report(False, label + " Content-Length", "missing")
    elif cl != str(len(expect_body)):
        ok = False
        report(False, label + " Content-Length", "got %s want %d" %
               (cl, len(expect_body)))
    else:
        report(True, label + " Content-Length: " + cl)

    if "server" not in headers or not headers["server"]:
        ok = False
        report(False, label + " Server header", "missing")
    else:
        report(True, label + " Server: " + headers["server"])

    if "date" not in headers:
        ok = False
        report(False, label + " Date header", "missing")
    else:
        dval = headers["date"]
        if not RFC1123.match(dval):
            ok = False
            report(False, label + " Date format", "got %r" % dval)
        else:
            then = parsedate_to_datetime(dval)
            delta = abs(time.time() - then.timestamp())
            if delta > 60:
                ok = False
                report(False, label + " Date freshness",
                       "%s differs from now by %.0fs" % (dval, delta))
            else:
                report(True, label + " Date: " + dval)
    return ok


def get(path, extra=b""):
    sock = connect()
    try:
        req = ("GET %s HTTP/1.1\r\nHost: %s\r\n%s"
               "Connection: close\r\n\r\n" % (path, HOST, extra)).encode()
        sock.sendall(req)
        buf = b""
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                return None, "timed out"
            if not chunk:
                break
            buf += chunk
        got, _ = read_response(sock, buf)
        if got is None or (isinstance(got, tuple) and got[0] == "ERR"):
            return None, got[1] if got else "no response"
        return got, None
    finally:
        sock.close()


def endpoint_checks(path, body, ctype, key_check=False):
    got, err = get(path)
    if got is None:
        report(False, path + " request", err)
        return
    status, headers, rbody, _ = got
    label = "GET " + path
    report(status == "200", label + " status", "got " + status)
    if rbody != body:
        report(False, label + " body bytes", "got %r want %r" %
               (rbody, body))
    else:
        report(True, label + " body bytes exact (%d B)" % len(body))
    check_headers(label, headers, ctype, body)
    if key_check:
        has_key = b'"message"' in rbody
        wrong = b'"Message"' in rbody or b'"MESSAGE"' in rbody
        report(has_key and not wrong, label + ' key case is exactly "message"')


print("tfb_validate: target %s:%s" % (HOST, PORT))

# 1 and 2: endpoint compliance.
endpoint_checks("/plaintext", PLAIN_BODY, "text/plain")
endpoint_checks("/json", JSON_BODY, "application/json", key_check=True)

# 3: unknown path answers 404.
got, err = get("/tfb-validator-missing")
if got is None:
    report(False, "404 request", err)
else:
    status, headers, rbody, _ = got
    report(status == "404", "GET unknown path returns 404",
           "got " + status)

# 4: keep-alive. Two sequential requests on one connection.
sock = connect()
try:
    buf = b""
    for i in (1, 2):
        sock.sendall(b"GET /plaintext HTTP/1.1\r\nHost: v\r\n\r\n")
        got, buf = read_response(sock, buf)
        if isinstance(got, tuple) and (got[0] == "ERR" or got is None):
            report(False, "keep-alive request %d" % i,
                   got[1] if got else "no response")
            break
        status, headers, rbody, buf = got
        report(status == "200" and rbody == PLAIN_BODY,
               "keep-alive request %d on one connection" % i)
except OSError as exc:
    report(False, "keep-alive connection", str(exc))
finally:
    sock.close()

# 5: pipelining. 100 GETs at once, then read 100 ordered responses.
N = 100
sock = connect()
try:
    one = b"GET /plaintext HTTP/1.1\r\nHost: v\r\n\r\n"
    sock.sendall(one * N)
    buf = b""
    counted = 0
    good = 0
    while counted < N:
        got, buf = read_response(sock, buf)
        if got is None or (isinstance(got, tuple) and got[0] == "ERR"):
            report(False, "pipelined response %d" % (counted + 1),
                   got[1] if got else "no more data")
            break
        status, headers, rbody, buf = got
        counted += 1
        if status == "200" and rbody == PLAIN_BODY:
            good += 1
        else:
            report(False, "pipelined response %d content" % counted,
                   "status=%s body=%r" % (status, rbody[:40]))
    report(good == N, "pipelined %d GETs return %d ordered 200 responses" %
           (N, N), "got %d/%d complete" % (counted, N))
except OSError as exc:
    report(False, "pipelined connection", str(exc))
finally:
    sock.close()

# 5b: mixed pipelining across both endpoints keeps the bodies aligned.
sock = connect()
try:
    sock.sendall(b"GET /json HTTP/1.1\r\nHost: v\r\n\r\n" * 5 +
                 b"GET /plaintext HTTP/1.1\r\nHost: v\r\n\r\n" * 5)
    buf = b""
    seq = [JSON_BODY] * 5 + [PLAIN_BODY] * 5
    good = 0
    for i, want in enumerate(seq):
        got, buf = read_response(sock, buf)
        if got is None or (isinstance(got, tuple) and got[0] == "ERR"):
            report(False, "mixed pipeline response %d" % (i + 1),
                   got[1] if got else "no more data")
            break
        status, headers, rbody, buf = got
        if status == "200" and rbody == want:
            good += 1
        else:
            report(False, "mixed pipeline response %d" % (i + 1),
                   "body=%r" % rbody[:40])
    report(good == 10, "mixed 10-request pipeline keeps bodies in order",
           "got %d/10" % good)
except OSError as exc:
    report(False, "mixed pipeline connection", str(exc))
finally:
    sock.close()

# 6: optional legacy reply for the wrk harness.
if CHECK_LEGACY:
    got, err = get("/")
    if got is None:
        report(False, "legacy / request", err)
    else:
        status, headers, rbody, _ = got
        report(status == "200" and rbody == b"Hello, World!\n",
               "legacy / keeps the 14-byte wrk body",
               "status=%s body=%r" % (status, rbody[:20]))

if failures:
    print("tfb_validate: %d check(s) FAILED" % failures)
    sys.exit(1)
print("tfb_validate: all checks passed")
PYEOF
rc=$?
exit $rc