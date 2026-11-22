# Anycubic LAN reads for the dashboard app. The slicer does not forward these.

import hashlib
import http.client
import json
import os
import socket
import ssl
import subprocess
import time
from base64 import b64decode
from secrets import token_hex
from urllib.parse import urlparse

LAN_PORT = 18910
MQTT_PORT = 9883
CLIENT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client_id.txt")


def client_id():
    existing = os.environ.get("DASHBOARD_CLIENT_ID", "").strip()
    if existing:
        return existing
    try:
        saved = open(CLIENT_PATH, "r", encoding="utf-8").read().strip()
        if len(saved) >= 16:
            return saved
    except OSError:
        pass
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    raw = token_hex(32)
    value = "".join(alphabet[int(raw[i:i + 2], 16) % 36] for i in range(0, 64, 2))
    try:
        open(CLIENT_PATH, "w", encoding="utf-8").write(value)
    except OSError:
        pass
    return value


def md5_hex(text):
    return hashlib.md5(text.encode("utf-8")).hexdigest()


def sign_request(token, timestamp, nonce):
    return md5_hex(md5_hex(token[:16]) + str(timestamp) + nonce)


def http_json(url, data=None, timeout=8):
    parsed = urlparse(url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=timeout)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    try:
        if data is None:
            connection.request("GET", path, headers={"Connection": "close"})
        else:
            connection.request("POST", path, body=data, headers={"Connection": "close"})
        response = connection.getresponse()
        payload = response.read()
    finally:
        connection.close()
    return json.loads(payload.decode("utf-8", "replace"))


def aes_decrypt(cipher, key, iv):
    if len(key) < 16 or len(iv) < 16 or not cipher:
        return None
    try:
        result = subprocess.run(
            [
                "openssl", "enc", "-d", "-aes-128-cbc",
                "-K", key[:16].encode("utf-8").hex(),
                "-iv", iv[:16].encode("utf-8").hex(),
            ],
            input=cipher,
            capture_output=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.decode("utf-8", "replace")


def open_session(host):
    info = http_json("http://{0}:{1}/info".format(host, LAN_PORT))
    token = str(info.get("token") or "")
    if len(token) < 32:
        raise RuntimeError("The printer did not return a LAN token.")
    timestamp = int(time.time() * 1000)
    nonce = token_hex(3)
    ctrl = info.get("ctrlInfoUrl") or "http://{0}:{1}/ctrl".format(host, LAN_PORT)
    join = "&" if "?" in ctrl else "?"
    url = "{0}{1}ts={2}&nonce={3}&sign={4}&did={5}".format(
        ctrl, join, timestamp, nonce, sign_request(token, timestamp, nonce), client_id()
    )
    reply = http_json(url, data=b"")
    data = reply.get("data") if isinstance(reply.get("data"), dict) else reply
    plain = aes_decrypt(b64decode(str(data.get("info") or "")), token[16:32], str(data.get("token") or ""))
    if not plain:
        raise RuntimeError("The printer's LAN handshake could not be read.")
    credentials = json.loads(plain)
    if isinstance(credentials.get("data"), dict):
        credentials = credentials["data"]
    session = {
        "host": host,
        "model_id": str(credentials.get("modeId") or credentials.get("modelId") or info.get("modelId") or ""),
        "device_id": str(credentials.get("deviceId") or ""),
        "username": str(credentials.get("username") or ""),
        "password": str(credentials.get("password") or ""),
    }
    if not session["model_id"] or not session["device_id"]:
        raise RuntimeError("The printer's LAN handshake did not include a device id.")
    return session


def mqtt_string(value):
    raw = value.encode("utf-8")
    return bytes((len(raw) >> 8, len(raw) & 0xFF)) + raw


def remaining(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            byte |= 0x80
        out.append(byte)
        if not length:
            break
    return bytes(out)


def mqtt_connect(client, username, password):
    variable = mqtt_string("MQTT") + bytes((4, 0xC2, 0, 30))
    variable += mqtt_string(client) + mqtt_string(username) + mqtt_string(password)
    return bytes((0x10,)) + remaining(len(variable)) + variable


def mqtt_publish(topic, payload):
    variable = mqtt_string(topic) + payload.encode("utf-8")
    return bytes((0x30,)) + remaining(len(variable)) + variable


def mqtt_subscribe(topic):
    variable = bytes((0, 1)) + mqtt_string(topic) + bytes((0,))
    return bytes((0x82,)) + remaining(len(variable)) + variable


def control_message(kind, action, data):
    return json.dumps({
        "type": kind,
        "action": action,
        "timestamp": int(time.time() * 1000),
        "msgid": "{0}-{1}-{2}-{3}-{4}".format(token_hex(4), token_hex(2), token_hex(2), token_hex(2), token_hex(6)),
        "data": json.loads(data),
    }, separators=(",", ":"))


def topic(session, kind):
    return "anycubic/anycubicCloud/v1/web/printer/{0}/{1}/{2}".format(
        session["model_id"], session["device_id"], kind
    )


class Broker:
    def __init__(self, session):
        raw = socket.create_connection((session["host"], MQTT_PORT), 8)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        self.socket = context.wrap_socket(raw, server_hostname=session["host"])
        self.socket.settimeout(8)
        self.buffer = b""

    def send(self, packet):
        self.socket.sendall(packet)

    def read(self, size):
        while len(self.buffer) < size:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("The printer closed the connection.")
            self.buffer += chunk
        got = self.buffer[:size]
        self.buffer = self.buffer[size:]
        return got

    def packet(self):
        header = self.read(1)[0]
        value = 0
        multiplier = 1
        for _ in range(4):
            digit = self.read(1)[0]
            value += (digit & 0x7F) * multiplier
            if (digit & 0x80) == 0:
                break
            multiplier *= 128
        body = self.read(value) if value else b""
        return header, body

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


def publish_queries(session, kinds):
    broker = Broker(session)
    try:
        client = "sia" + token_hex(4)
        broker.send(mqtt_connect(client, session["username"], session["password"]))
        header, body = broker.packet()
        if header != 0x20 or len(body) < 2 or body[-1] != 0:
            raise RuntimeError("The printer did not accept the command.")
        reports = "anycubic/anycubicCloud/v1/printer/public/{0}/{1}/#".format(
            session["model_id"], session["device_id"]
        )
        broker.send(mqtt_subscribe(reports))
        for kind, action, data in kinds:
            broker.send(mqtt_publish(topic(session, kind), control_message(kind, action, data)))
        broker.socket.settimeout(3)
        messages = []
        for _ in range(24):
            try:
                header, body = broker.packet()
            except (socket.timeout, TimeoutError, ssl.SSLError):
                break
            if (header & 0xF0) != 0x30 or len(body) < 2:
                continue
            topic_len = (body[0] << 8) | body[1]
            cursor = 2 + topic_len
            if ((header >> 1) & 0x03) > 0:
                cursor += 2
            if cursor <= len(body):
                messages.append(body[cursor:].decode("utf-8", "replace"))
        return messages
    finally:
        broker.close()


def walk(node, found):
    if isinstance(node, dict):
        found.append(node)
        for value in node.values():
            walk(value, found)
    elif isinstance(node, list):
        for value in node:
            walk(value, found)


def first_int(nodes, key):
    for node in nodes:
        if key in node and isinstance(node[key], (int, float)):
            return int(node[key])
    return None


def color_hex(color):
    if isinstance(color, str):
        text = color[1:] if color.startswith("#") else color
        if len(text) >= 6:
            try:
                int(text[:6], 16)
            except ValueError:
                return ""
            return "#" + text[:6].lower()
        return ""
    if isinstance(color, list) and color and isinstance(color[0], list):
        return color_hex(color[0])
    if isinstance(color, list) and len(color) >= 3 and all(isinstance(item, (int, float)) for item in color[:3]):
        return "#{:02x}{:02x}{:02x}".format(int(color[0]), int(color[1]), int(color[2]))
    return ""


def read_ace(nodes):
    slots = []
    for node in nodes:
        boxes = node.get("multi_color_box")
        if not isinstance(boxes, list):
            continue
        for box in boxes:
            if not isinstance(box, dict):
                continue
            box_id = box.get("id")
            if not isinstance(box_id, int) or box_id < 0:
                continue
            if box.get("status") == 0:
                continue
            for slot in box.get("slots") or []:
                if not isinstance(slot, dict):
                    continue
                index = slot.get("index")
                if not isinstance(index, int):
                    index = 0
                material = slot.get("type") or slot.get("material") or ""
                if not isinstance(material, str):
                    material = ""
                color = color_hex(slot.get("color")) or color_hex(slot.get("colors"))
                edit_status = slot.get("edit_status", 1)
                empty = edit_status == 2 and not color and not material
                if empty and not color and not material:
                    slots.append({"box": box_id, "index": index, "material": "", "color": "", "empty": True})
                    continue
                if not color and not material:
                    continue
                slots.append({
                    "box": box_id,
                    "index": index,
                    "material": material,
                    "color": color,
                    "empty": False,
                })
    for box_id in {slot["box"] for slot in slots}:
        group = [slot for slot in slots if slot["box"] == box_id]
        if any(slot["index"] == 0 for slot in group):
            continue
        for slot in group:
            if slot["index"] > 0:
                slot["index"] -= 1
    return slots


def read_light(nodes):
    found = None
    for node in nodes:
        lights = node.get("lights")
        if isinstance(lights, list):
            for item in lights:
                if isinstance(item, dict) and item.get("type") == 2 and isinstance(item.get("status"), int):
                    found = item["status"] != 0
        if node.get("type") == 2 and isinstance(node.get("status"), int) and "brightness" in node:
            found = node["status"] != 0
    return found


def read_printer(host):
    session = open_session(host)
    messages = publish_queries(session, [
        ("info", "query", "null"),
        ("print", "query", "null"),
        ("tempature", "query", "{}"),
        ("fan", "query", "{}"),
        ("light", "query", "{}"),
        ("multiColorBox", "getInfo", "{}"),
    ])
    nodes = []
    for message in messages:
        try:
            walk(json.loads(message), nodes)
        except json.JSONDecodeError:
            continue
    nozzle = first_int(nodes, "curr_nozzle_temp")
    bed = first_int(nodes, "curr_hotbed_temp")
    chamber = first_int(nodes, "curr_chamber_temp")
    model_fan = first_int(nodes, "fan_speed_pct")
    aux_fan = first_int(nodes, "aux_fan_speed_pct")
    progress = first_int(nodes, "progress")
    layer = first_int(nodes, "curr_layer")
    layers = first_int(nodes, "total_layers")
    remain = first_int(nodes, "remain_time")
    elapsed = first_int(nodes, "print_time")
    filename = ""
    printing = False
    for node in nodes:
        if isinstance(node.get("filename"), str) and node["filename"]:
            filename = node["filename"]
        state = node.get("state")
        if isinstance(state, str) and state.lower() in ("printing", "busy", "paused", "heating", "preheating"):
            printing = True
        if isinstance(state, int) and state in (1, 2):
            printing = True
    if progress is not None and 0 < progress < 100:
        printing = True
    def degree(value):
        return "{0}°C".format(value) if value is not None else "—"

    def percent(value):
        return "{0}%".format(value) if value is not None else "—"

    light = read_light(nodes)
    if nozzle is None and bed is None and chamber is None and not filename and not nodes:
        raise RuntimeError("The printer did not report its status.")
    return {
        "status": "Printing" if printing else "Ready",
        "job": filename,
        "light": light,
        "ace": read_ace(nodes),
        "readings": {
            "nozzle": degree(nozzle),
            "bed": degree(bed),
            "chamber": degree(chamber),
            "model_fan": percent(model_fan),
            "aux_fan": percent(aux_fan),
            "light": "On" if light else "Off" if light is not None else "—",
        },
        "task": {
            "known": True,
            "printing": printing,
            "name": filename,
            "progress": progress if progress is not None else -1,
            "layer": layer if layer is not None else -1,
            "layers": layers if layers is not None else -1,
            "remain": remain if remain is not None else -1,
            "elapsed": elapsed if elapsed is not None else -1,
        },
    }


def publish_control(host, messages):
    session = open_session(host)
    broker = Broker(session)
    try:
        broker.send(mqtt_connect("sia" + token_hex(4), session["username"], session["password"]))
        header, body = broker.packet()
        if header != 0x20 or len(body) < 2 or body[-1] != 0:
            raise RuntimeError("The printer did not accept the command.")
        for kind, action, data in messages:
            broker.send(mqtt_publish(topic(session, kind), control_message(kind, action, data)))
    finally:
        broker.close()


def camera_command(host, start):
    publish_control(host, [("video", "startCapture" if start else "stopCapture", "{}")])


def set_light(host, on):
    data = json.dumps({"type": 2, "status": 1 if on else 0, "brightness": 100 if on else 0}, separators=(",", ":"))
    publish_control(host, [("light", "control", data)])


def camera_jpeg(host):
    result = subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-i", "http://{0}:18088/flv".format(host),
            "-frames:v", "1", "-f", "image2", "-vcodec", "mjpeg", "pipe:1",
        ],
        capture_output=True,
        timeout=8,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.startswith(b"\xff\xd8"):
        raise RuntimeError("The camera did not answer.")
    return result.stdout
