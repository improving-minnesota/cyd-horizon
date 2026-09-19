#!/usr/bin/env python3
"""Push a local dev build to the CYD over WiFi (triggered local OTA).

This is the default way to iterate once a dev build with ENABLE_LOCAL_OTA +
ENABLE_SERIAL_PROVISION is running on the device - much faster than
`arduino-cli upload` (~80 s at 115200 baud) and it leaves no stale binary
questions. USB flashing is only for the FIRST dev flash (or a board whose
flash is blank).

What it does:
  1. Starts a local HTTP server serving the build output directory.
  2. Opens the serial console and pulses RTS to force a fresh boot, then
     waits for the "[net] ip=" line so the WiFi link is up before
     triggering - avoiding the code=-1 connect race described in
     DEVELOPER.md ("Avoid a reset before sending the command"). If the boot
     log reports a release build ("[boot] version=" without -dev), the push
     falls back to a one-time USB flash of the same build dir (release
     firmware has no serial-OTA listener) - the flashed dev build accepts
     OTA from then on.
  3. Sends OTA_VER=<version label> (extracted from the binary's embedded
     CYD_TAG= string) so the OTA screen shows the real version, then
     OTA_URL=http://<host-ip>:<http-port>/<bin>, and streams the
     [OTA]/CMD progress lines until the device reboots into the new image.

Usage (from the repo root):
  cyd-horizon/.venv/bin/python scripts/ota_push.py \
      [--port /dev/cu.usbserial-XXXX] [--dir build/release] \
      [--file cyd-horizon.ino.bin] [--http-port 8080] \
      [--board e32r40t|2432s028r] [--all]

With several boards plugged in, --board picks the right one via
scripts/detect_boards.py (the firmware's "[boot] board=" marker). The board
is also inferred from --dir names like "release-e32r40t", so a plain
`--dir build/release-e32r40t` already targets the 4" board.

--all updates every detected board concurrently: each board pulls its own
variant's binary in parallel (the HTTP server is threaded, so parallel pulls
are fine). Variant dirs are the convention build/release for 2432s028r and
build/release-<board> for others, relative to --dir's parent.
"""

import argparse
import functools
import glob
import http.server
import os
import re
import socket
import socketserver
import sys
import threading
import time

import serial
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import detect_boards
import flash

BAUD = 115200
WIFI_WAIT_S = 45      # after reset, wait this long for "[net] ip="
OTA_WATCH_S = 150     # stream [OTA] progress this long after triggering
KNOWN_BOARDS = ("e32r40t", "2432s028r")


def infer_board(bindir):
    """Board variant implied by the build dir name, e.g. release-e32r40t.
    The bare "release" dir is the documented 2.8" convention."""
    base = os.path.basename(os.path.normpath(bindir)).lower()
    if base == "release":
        return "2432s028r"
    for b in KNOWN_BOARDS:
        if b in base:
            return b
    return None


def find_port(cli_port, want_board):
    if cli_port:
        return cli_port
    ports = sorted(glob.glob("/dev/cu.usbserial*"))
    if len(ports) == 1 and not want_board:
        return ports[0]
    if not ports:
        sys.exit("no /dev/cu.usbserial-* ports found - is a board plugged in?")

    print("detecting boards on %d port(s)..." % len(ports))
    info = detect_boards.probe_ports(ports, detect_boards.PROBE_S,
                                     want_ip=False)
    for p in ports:
        print("  %s -> %s" % (p, info.get(p, {}).get("board", "unknown")))

    matches = [p for p in ports
               if info.get(p, {}).get("board") == want_board]
    if want_board:
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            sys.exit("several %s boards found %r - pass --port"
                     % (want_board, matches))
        sys.exit("no port reported board=%s - pass --port, or the firmware "
                 "predates the board marker (flash once with --port)"
                 % want_board)
    if len(ports) == 1:
        return ports[0]
    sys.exit("serial port ambiguous; found %r - pass --port or --board"
             % ports)


def build_tag(binpath):
    """Extract the CYD_TAG=... string the firmware embeds in the binary so the
    device's OTA screen can name the version being installed (e.g.
    "1.18.0-dev, Build 23")."""
    data = open(binpath, "rb").read()
    m = re.search(rb"CYD_TAG=([^\x00]+)", data)
    return m.group(1).decode("utf-8", "replace") if m else None


def host_ip_for(peer_ip):
    """Our LAN IP on the subnet that can reach the board (peer_ip)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer_ip, 80))   # no traffic is actually sent
        return s.getsockname()[0]
    finally:
        s.close()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def start_server(directory, port):
    handler = functools.partial(QuietHandler, directory=directory)
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("", port), handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


_plock = threading.Lock()

def log(board, msg):
    """Serial output from concurrent board jobs; the lock keeps lines whole."""
    with _plock:
        print(("  [%s] %s" % (board, msg)) if board else ("  " + msg),
              flush=True)


def usb_flash(port, board, input_dir, label):
    """USB-upload input_dir to the board on `port` - the fallback for boards
    running release firmware, which has no serial-OTA listener. Mirrors
    flash.py's invocation (per-board baud; the E32R40T's CH340 can't take
    921600). Returns the result string recorded for this port."""
    if not input_dir or not os.path.isdir(input_dir):
        return "release build; no build dir to USB-flash"
    speed = flash.BOARDS.get(board, {}).get("speed", 115200)
    cmd = ["arduino-cli", "upload", "-b", flash.FQBN,
           "--input-dir", input_dir, "-p", port,
           "--upload-property", "upload.speed=%d" % speed]
    log(label, "+ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            errors="replace")
    for line in proc.stdout:
        log(label, line.rstrip())
    proc.wait()
    if proc.returncode == 0:
        log(label, "flashed over USB - this dev build accepts the next OTA")
        return "ok (usb flash)"
    return "USB flash failed (exit %d)" % proc.returncode


def run_ota(port, board, url_path, tag, http_port, results, flash_dir=None):
    """Reset one board, wait for its [net] ip=, send OTA_VER/OTA_URL for
    url_path (relative to the served dir), and watch until it reboots into
    the new image. A board running release firmware is USB-flashed from
    flash_dir instead. Runs standalone or as a worker thread in --all mode;
    result is recorded in results[port]."""
    label = board if board else None
    ser = None
    try:
        log(label, "serial %s @ %d (resetting board; waiting for WiFi)"
            % (port, BAUD))
        ser = serial.Serial(port, BAUD, timeout=0.25)
        # Force a fresh boot for the "[net] ip=" line (only prints right after
        # WiFi connects); DTR stays released so it boots normally.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False

        sent = False
        deadline = time.time() + WIFI_WAIT_S
        ota_deadline = None
        buf = b""
        while True:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    log(label, line)
                    # A release build never prints "[net] ip=" and ignores OTA
                    # commands - USB-flash it instead of the WiFi timeout.
                    # (Pre-trigger only: post-`sent`, version= marks the new
                    # image's boot.)
                    mv = re.search(r"\[boot\] version=(\S+)", line)
                    if mv and not sent and "-dev" not in mv.group(1):
                        log(label, "board runs release build %s - "
                                   "USB-flashing instead" % mv.group(1))
                        ser.close()   # free the port for esptool
                        ser = None
                        results[port] = usb_flash(port, board, flash_dir,
                                                  label)
                        return
                    m = re.search(r"\[net\] ip=(\S+)", line)
                    if m and not sent:
                        board_ip = m.group(1)
                        my_ip = host_ip_for(board_ip)
                        url = "http://%s:%d/%s" % (my_ip, http_port, url_path)
                        log(label, "board at %s; triggering OTA from %s"
                            % (board_ip, url))
                        if tag:
                            ser.write(("OTA_VER=" + tag + "\n").encode())
                            time.sleep(0.1)
                        ser.write(("OTA_URL=" + url + "\n").encode())
                        sent = True
                        ota_deadline = time.time() + OTA_WATCH_S
                    if sent and re.search(r"\[boot\] version=", line):
                        results[port] = "ok"
                        log(label, "OTA complete - device rebooted into the "
                                   "new image.")
                        return
                    if "update failed" in line:
                        results[port] = "OTA failed on device"
                        return
            if not sent and time.time() > deadline:
                results[port] = ("timed out waiting for WiFi ([net] ip=) "
                                 "after reset")
                return
            if sent and ota_deadline and time.time() > ota_deadline:
                results[port] = "timed out watching OTA"
                return
    except Exception as e:
        results[port] = "error: %s" % e
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


def variant_subdir(board):
    """Build dir (under build/) holding a board's binary: the bare "release"
    dir is the documented 2432s028r convention, others use release-<board>."""
    return "release" if board == "2432s028r" else "release-" + board


def push_all(args):
    """Update every detected board in parallel, each with its own variant's
    binary. Serves the build root so per-board URLs are <subdir>/<file>."""
    build_root = os.path.abspath(os.path.join(args.dir, os.pardir))
    ports = sorted(glob.glob("/dev/cu.usbserial*"))
    if not ports:
        sys.exit("no /dev/cu.usbserial-* ports found - is a board plugged in?")

    print("detecting boards on %d port(s)..." % len(ports))
    info = detect_boards.probe_ports(ports, detect_boards.PROBE_S,
                                     want_ip=False)

    jobs = []   # (port, board, url_path, tag, flash_dir)
    for p in ports:
        board = info.get(p, {}).get("board", "unknown")
        ver = info.get(p, {}).get("version")
        print("  %s -> %s%s" % (p, board,
                                (" v" + ver) if ver else ""))
        if args.board and board != args.board:
            continue
        if board not in KNOWN_BOARDS:
            print("  skipping %s (unknown board - can't pick a binary)" % p)
            continue
        # Release firmware has no serial-OTA listener; run_ota USB-flashes
        # those boards instead. Boards too old to print version= fall through
        # to the WiFi wait.
        if ver and "-dev" not in ver:
            print("  %s runs release %s - will USB-flash" % (p, ver))
        flash_dir = os.path.join(build_root, variant_subdir(board))
        binpath = os.path.join(flash_dir, args.file)
        if not os.path.isfile(binpath):
            print("  skipping %s (no image: %s)" % (p, binpath))
            continue
        jobs.append((p, board, variant_subdir(board) + "/" + args.file,
                     build_tag(binpath), flash_dir))

    if not jobs:
        sys.exit("nothing to update")
    print("serving %s on http port %d; updating %d board(s)"
          % (build_root, args.http_port, len(jobs)))
    start_server(build_root, args.http_port)

    results = {}
    threads = [threading.Thread(target=run_ota,
                                args=(p, b, path, tag, args.http_port, results,
                                      fdir),
                                daemon=True)
               for (p, b, path, tag, fdir) in jobs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    failed = False
    for (p, b, _, _, _) in jobs:
        res = results.get(p, "no result")
        print("%s (%s): %s" % (p, b, res))
        failed |= not res.startswith("ok")
    if failed:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--port", help="serial port (default: sole /dev/cu.usbserial*)")
    ap.add_argument("--dir", default=os.path.join(here, "..", "build", "release"),
                    help="directory containing the .bin (default: ../build/release)")
    ap.add_argument("--file", default="cyd-horizon.ino.bin", help="bin filename")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--board", choices=KNOWN_BOARDS,
                    help="board variant to target (default: inferred from "
                         "--dir, or sole connected board); with --all, "
                         "limits the update to that variant")
    ap.add_argument("--all", action="store_true",
                    help="update every detected board in parallel, each with "
                         "its variant's binary from build/release*")
    args = ap.parse_args()

    if args.all:
        push_all(args)
        return

    bindir = os.path.abspath(args.dir)
    binpath = os.path.join(bindir, args.file)
    if not os.path.isfile(binpath):
        sys.exit("no such image: %s (compile first)" % binpath)
    want_board = args.board or infer_board(bindir)
    if want_board:
        print("target board: %s" % want_board)
    print("serving %s on http port %d" % (binpath, args.http_port))
    start_server(bindir, args.http_port)

    port = find_port(args.port, want_board)
    tag = build_tag(binpath)
    if tag:
        print("image label: %s" % tag)

    results = {}
    run_ota(port, want_board, args.file, tag, args.http_port, results, bindir)
    res = results.get(port)
    if not (res or "").startswith("ok"):
        sys.exit(res or "OTA did not complete")


if __name__ == "__main__":
    main()
