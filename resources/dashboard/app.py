# Slice it All dashboard app.
#
# Slice it All sends the layout, the printer addresses, and the details of
# printers that have no network connection. This app reads every networked
# printer itself. TrueNAS needs host networking so the announcement is heard:
#
#   docker run --network host sliceitall-dashboard
#
# Point the Cloudflare tunnel at http://<this-server>:8080
# Set DASHBOARD_HOST when the announcement must name a specific LAN address.

import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from secrets import token_hex
from urllib.parse import parse_qs, unquote, urlparse

import anycubic

PORT = int(os.environ.get("DASHBOARD_PORT", "8080"))
ADVERTISE = os.environ.get("DASHBOARD_HOST", "").strip()
TOKEN = os.environ.get("DASHBOARD_TOKEN", token_hex(16))
HERE = Path(__file__).resolve().parent
PAGE = (HERE / "index.html").read_text(encoding="utf-8")

lock = threading.Lock()
layout = {"tiles": [], "printers": [], "modules": {}}
live = {}
live_error = {}
wanted = {}
camera_note = {}
frames = {}


def beacon_loop():
    payload = "sliceitall-dashboard\n{port}\n{token}\n{host}\n".format(
        port=PORT, token=TOKEN, host=ADVERTISE
    ).encode("utf-8")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        try:
            sock.sendto(payload, ("255.255.255.255", 8766))
        except OSError:
            pass
        time.sleep(2)


def printer_by_name(name):
    for printer in layout.get("printers") or []:
        if printer.get("name") == name:
            return printer
    return None


def dashboard_body():
    devices = []
    for printer in layout.get("printers") or []:
        name = printer.get("name") or ""
        device = {
            "name": name,
            "brand": printer.get("brand") or "",
            "host": printer.get("host") or "",
            "camera": bool(printer.get("camera")),
            "offline": bool(printer.get("offline")),
        }
        if printer.get("offline") or not printer.get("host"):
            device["status"] = printer.get("status") or "Offline"
            device["job"] = printer.get("job") or ""
            device["materials"] = printer.get("materials")
        elif printer.get("link") != "Anycubic LAN":
            device["status"] = "Idle"
            device["job"] = "This connection is not read by the dashboard app yet."
        else:
            reading = live.get(name)
            if reading:
                device["status"] = reading.get("status") or "Ready"
                device["job"] = reading.get("job") or ""
                device["temperatures"] = reading.get("temperatures") or ""
                if reading.get("materials"):
                    device["materials"] = reading["materials"]
            else:
                device["status"] = "Offline" if name in live_error else "Idle"
                device["job"] = live_error.get(name) or ""
        devices.append(device)
    return json.dumps({
        "devices": devices,
        "tiles": layout.get("tiles") or [],
        "modules": layout.get("modules") or {},
    }, ensure_ascii=False).encode("utf-8")


def poll_loop():
    camera_started = set()
    while True:
        with lock:
            printers = [dict(item) for item in (layout.get("printers") or [])
                        if item.get("host") and not item.get("offline") and item.get("link") == "Anycubic LAN"]
            cameras = dict(wanted)
        for printer in printers:
            name = printer.get("name") or ""
            host = printer.get("host") or ""
            try:
                reading = anycubic.read_printer(host)
                with lock:
                    live[name] = reading
                    live_error.pop(name, None)
            except Exception as error:
                with lock:
                    live.pop(name, None)
                    live_error[name] = str(error)
        for name, action in cameras.items():
            printer = None
            with lock:
                printer = printer_by_name(name)
            if not printer or not printer.get("host"):
                continue
            host = printer["host"]
            try:
                if action == "stop":
                    if name in camera_started:
                        anycubic.camera_command(host, False)
                        camera_started.discard(name)
                    with lock:
                        frames.pop(name, None)
                        camera_note[name] = {"ok": True, "running": False, "note": "Camera is stopped."}
                    continue
                if name not in camera_started:
                    anycubic.camera_command(host, True)
                    camera_started.add(name)
                picture = anycubic.camera_jpeg(host)
                with lock:
                    frames[name] = picture
                    camera_note[name] = {"ok": True, "running": True, "note": ""}
            except Exception as error:
                with lock:
                    camera_note[name] = {"ok": False, "running": False, "note": str(error)}
        time.sleep(4)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def _send(self, code, body, content_type, reason="OK"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code, reason)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _read(self, limit):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length < 0 or length > limit:
            return None
        return self.rfile.read(length) if length else b""

    def _authorized(self):
        return self.headers.get("Authorization", "") == "Bearer " + TOKEN

    def _device(self):
        query = parse_qs(urlparse(self.path).query)
        return unquote((query.get("device") or [""])[0])

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._send(200, PAGE, "text/html; charset=utf-8")
            return
        if path == "/api/dashboard":
            with lock:
                body = dashboard_body()
            self._send(200, body, "application/json; charset=utf-8")
            return
        if path == "/api/camera":
            device = self._device()
            action = (parse_qs(urlparse(self.path).query).get("action") or ["start"])[0]
            with lock:
                wanted[device] = "stop" if action == "stop" else "start"
                note = camera_note.get(device)
            if not note:
                note = {"ok": True, "running": False, "note": "Asking the printer."}
            self._send(200, json.dumps(note), "application/json; charset=utf-8")
            return
        if path == "/api/camera/frame":
            device = self._device()
            with lock:
                frame = frames.get(device)
            if not frame:
                self._send(204, b"", "text/plain; charset=utf-8", "No Content")
                return
            self._send(200, frame, "image/jpeg")
            return
        self._send(404, "Not found\n", "text/plain; charset=utf-8", "Not Found")

    def do_POST(self):
        path = urlparse(self.path).path
        if not self._authorized():
            self._send(401, "Unauthorized\n", "text/plain; charset=utf-8", "Unauthorized")
            return
        if path == "/api/push":
            body = self._read(1024 * 1024)
            try:
                incoming = json.loads(body.decode("utf-8"))
            except (UnicodeError, json.JSONDecodeError, AttributeError):
                incoming = None
            if not isinstance(incoming, dict) or "printers" not in incoming:
                self._send(400, "Bad request\n", "text/plain; charset=utf-8", "Bad Request")
                return
            with lock:
                global layout
                layout = {
                    "tiles": incoming.get("tiles") or [],
                    "printers": incoming.get("printers") or [],
                    "modules": incoming.get("modules") or {},
                }
                names = {item.get("name") for item in layout["printers"]}
                for name in list(live):
                    if name not in names:
                        live.pop(name, None)
                        live_error.pop(name, None)
            self._send(200, "ok\n", "text/plain; charset=utf-8")
            return
        self._send(404, "Not found\n", "text/plain; charset=utf-8", "Not Found")


def main():
    threading.Thread(target=beacon_loop, daemon=True).start()
    threading.Thread(target=poll_loop, daemon=True).start()
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("Slice it All dashboard on port {0}".format(PORT), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
