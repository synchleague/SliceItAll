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
import subprocess
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
lights = {}
feeds = {}


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


def overlay_ace(materials, ace):
    if not ace:
        return materials
    if not isinstance(materials, dict) or materials.get("kind") != "spools":
        bays = []
        grouped = {}
        for slot in ace:
            grouped.setdefault(slot.get("box"), []).append(slot)
        for box in sorted(box for box in grouped if box is not None):
            ordered = sorted(grouped[box], key=lambda item: item.get("index") or 0)
            bays.append({
                "title": "ACE1" if box == 0 else "ACE2" if box == 1 else "ACE",
                "drying": "",
                "slots": [{
                    "material": item.get("material") or "",
                    "color": item.get("color") or "",
                    "badge": str((item.get("index") or 0) + 1),
                    "filled": not item.get("empty") and bool(item.get("material") or item.get("color")),
                } for item in ordered],
            })
        return {"kind": "spools", "bays": bays} if bays else materials
    merged = json.loads(json.dumps(materials))
    bays = merged.setdefault("bays", [])
    for slot in ace:
        box = slot.get("box")
        title = "ACE1" if box == 0 else "ACE2" if box == 1 else None
        if not title:
            continue
        bay = next((item for item in bays if str(item.get("title") or "").upper() == title), None)
        if bay is None:
            bay = {"title": title, "drying": "", "slots": []}
            bays.insert(0, bay)
        row = bay.setdefault("slots", [])
        index = slot.get("index") or 0
        while len(row) <= index:
            row.append({"material": "", "color": "", "badge": str(len(row) + 1), "filled": False})
        current = row[index]
        if slot.get("empty") and not current.get("material") and not current.get("color"):
            current["material"] = ""
            current["color"] = ""
            current["filled"] = False
            continue
        if slot.get("color"):
            current["color"] = slot["color"]
        if slot.get("material"):
            current["material"] = slot["material"]
        current["filled"] = bool(current.get("material") or current.get("color"))
        current["badge"] = str(index + 1)
    return merged


class CameraFeed:
    def __init__(self, name, host):
        self.name = name
        self.host = host
        self.stop = threading.Event()
        self.proc = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        try:
            self.proc = subprocess.Popen(
                [
                    "ffmpeg", "-hide_banner", "-loglevel", "error",
                    "-fflags", "nobuffer", "-flags", "low_delay",
                    "-i", "http://{0}:18088/flv".format(self.host),
                    "-an", "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "8", "pipe:1",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            buf = b""
            while not self.stop.is_set():
                chunk = self.proc.stdout.read(8192)
                if not chunk:
                    break
                buf += chunk
                while True:
                    start = buf.find(b"\xff\xd8")
                    if start < 0:
                        buf = b""
                        break
                    end = buf.find(b"\xff\xd9", start + 2)
                    if end < 0:
                        buf = buf[start:]
                        break
                    picture = buf[start:end + 2]
                    buf = buf[end + 2:]
                    with lock:
                        frames[self.name] = picture
        except Exception as error:
            with lock:
                camera_note[self.name] = {"ok": False, "running": True, "note": str(error)}
        finally:
            if self.proc and self.proc.poll() is None:
                self.proc.kill()

    def close(self):
        self.stop.set()
        if self.proc and self.proc.poll() is None:
            self.proc.kill()


def printer_host(name):
    with lock:
        printer = printer_by_name(name)
    if not printer or not printer.get("host") or printer.get("offline"):
        return None
    return printer["host"]


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
        device["model"] = printer.get("model") or ""
        device["materials"] = printer.get("materials")
        if printer.get("offline") or not printer.get("host"):
            device["status"] = printer.get("status") or "Offline"
            device["job"] = printer.get("job") or ""
        elif printer.get("link") != "Anycubic LAN":
            device["status"] = "Idle"
            device["job"] = "This connection is not read by the dashboard app yet."
        else:
            reading = live.get(name)
            if reading:
                device["status"] = reading.get("status") or "Ready"
                device["job"] = reading.get("job") or ""
                device["readings"] = reading.get("readings") or {}
                if reading.get("task"):
                    device["task"] = reading["task"]
                device["materials"] = overlay_ace(device.get("materials"), reading.get("ace"))
                if isinstance(reading.get("light"), bool):
                    lights[name] = reading["light"]
            else:
                device["status"] = "Failed" if name in live_error else "Idle"
                device["job"] = live_error.get(name) or ""
            if name in lights:
                device["light"] = lights[name]
        devices.append(device)
    return json.dumps({
        "devices": devices,
        "tiles": layout.get("tiles") or [],
        "rows": layout.get("rows") or [],
        "modules": layout.get("modules") or {},
    }, ensure_ascii=False).encode("utf-8")


def poll_loop():
    while True:
        with lock:
            printers = [dict(item) for item in (layout.get("printers") or [])
                        if item.get("host") and not item.get("offline") and item.get("link") == "Anycubic LAN"]
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
            start = (parse_qs(urlparse(self.path).query).get("action") or ["start"])[0] != "stop"
            host = printer_host(device)
            if not host:
                self._send(200, json.dumps({"ok": False, "running": False, "note": "This printer has no network address, so the camera cannot be started."}), "application/json; charset=utf-8")
                return
            try:
                anycubic.camera_command(host, start)
                try:
                    anycubic.set_light(host, start)
                except Exception:
                    pass
            except Exception as error:
                note = {"ok": False, "running": False, "note": str(error)}
                with lock:
                    camera_note[device] = note
                self._send(200, json.dumps(note), "application/json; charset=utf-8")
                return
            with lock:
                if start:
                    previous = feeds.pop(device, None)
                    if previous:
                        previous.close()
                    feeds[device] = CameraFeed(device, host)
                    lights[device] = True
                    note = {"ok": True, "running": True, "light": True, "note": ""}
                else:
                    previous = feeds.pop(device, None)
                    if previous:
                        previous.close()
                    frames.pop(device, None)
                    lights[device] = False
                    note = {"ok": True, "running": False, "light": False, "note": "Camera is stopped. Press Start to ask this printer for a picture."}
                camera_note[device] = note
            self._send(200, json.dumps(note), "application/json; charset=utf-8")
            return
        if path == "/api/light":
            device = self._device()
            turn_on = (parse_qs(urlparse(self.path).query).get("on") or ["1"])[0] != "0"
            host = printer_host(device)
            if not host:
                self._send(200, json.dumps({"ok": False, "on": False, "note": "This printer has no network address."}), "application/json; charset=utf-8")
                return
            try:
                anycubic.set_light(host, turn_on)
            except Exception as error:
                self._send(200, json.dumps({"ok": False, "on": not turn_on, "note": str(error) or "The chamber light did not switch."}), "application/json; charset=utf-8")
                return
            with lock:
                lights[device] = turn_on
            self._send(200, json.dumps({"ok": True, "on": turn_on, "note": ""}), "application/json; charset=utf-8")
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
                    "rows": incoming.get("rows") or [],
                    "printers": incoming.get("printers") or [],
                    "modules": incoming.get("modules") or {},
                }
                names = {item.get("name") for item in layout["printers"]}
                for name in list(live):
                    if name not in names:
                        live.pop(name, None)
                        live_error.pop(name, None)
                for name in list(feeds):
                    if name not in names:
                        feeds.pop(name).close()
                        frames.pop(name, None)
                        lights.pop(name, None)
            self._send(200, "ok\n", "text/plain; charset=utf-8")
            return
        self._send(404, "Not found\n", "text/plain; charset=utf-8", "Not Found")


def main():
    threading.Thread(target=beacon_loop, daemon=True).start()
    threading.Thread(target=poll_loop, daemon=True).start()
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("Slice it All dashboard on port {0}".format(PORT), flush=True)
    print("Layout token: {0}".format(TOKEN), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
