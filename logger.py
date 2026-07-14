#!/usr/bin/env python3
"""门禁自动记录器 v2
- 监听 Uno 串口,每次刷卡(LOG,...)追加到 access_log.csv 并实时推送 Google Sheet
- 每次连上 Uno:自动把 Mac 的精确时间写进 DS1302(T命令),再把离线期间
  存在 EEPROM 里的记录补进 CSV + Google Sheet(d命令导出,按时间戳去重)
- 拔掉 Uno 自动等待重连;存在 /tmp/accesslogger.pause 时释放串口(烧录时用)
"""
import csv, glob, json, os, time, datetime
import urllib.request

import serial

CSV_PATH = os.path.expanduser("~/Projects/esp32-access-control/access_log.csv")
PAUSE_FLAG = "/tmp/accesslogger.pause"
HEADER = ["mac_time", "device_unix", "device_time", "name", "uid", "result"]

# 私密配置(webhook地址、卡号表)放在 config.json 里,不进 git —— 模板见 config.example.json
_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")
try:
    with open(_CONFIG_PATH) as _f:
        _cfg = json.load(_f)
except FileNotFoundError:
    raise SystemExit("缺少 config.json:请 cp config.example.json config.json 并填入你的 webhook 和卡号")
WEBHOOK = _cfg["webhook"]
UID_BY_NAME = _cfg["uid_by_name"]


def append_row(row):
    new = not os.path.exists(CSV_PATH)
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(HEADER)
        w.writerow(row)


def push_to_sheet(row):
    try:
        data = json.dumps(dict(zip(HEADER, row))).encode()
        req = urllib.request.Request(WEBHOOK, data=data,
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=10)
    except Exception:
        pass  # 断网也不影响本地CSV;下次补账会兜底


def known_device_ts():
    """CSV里已有的设备时间戳集合,用来给补账去重"""
    seen = set()
    if os.path.exists(CSV_PATH):
        with open(CSV_PATH) as f:
            for r in csv.DictReader(f):
                try:
                    ts = int(r.get("device_unix", "0"))
                    if ts > 0:
                        seen.add(ts)
                except ValueError:
                    pass
    return seen


def handle_log_line(line):
    parts = line.split(",")
    if len(parts) == 6:
        _, ts, dev_time, name, uid, result = parts
        mac_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        row = [mac_time, ts, dev_time, name, uid, result]
        append_row(row)
        push_to_sheet(row)


def sync_time(ser):
    """把Mac的本地时间写进DS1302(用'本地墙钟当UTC'的epoch,与固件显示口径一致)"""
    wall = int(datetime.datetime.now().replace(tzinfo=datetime.timezone.utc).timestamp())
    ser.write(f"T{wall}\n".encode())
    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "TIMESET,OK" in line:
            return True
        if line.startswith("LOG,"):
            handle_log_line(line)
    return False


def reconcile(ser):
    """把EEPROM里离线期间的记录补进CSV+表格(按时间戳去重)"""
    seen = known_device_ts()
    ser.write(b"d")
    added = 0
    deadline = time.time() + 30
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line == "DUMP,END":
            break
        if line.startswith("LOG,"):
            handle_log_line(line)
            continue
        if line.startswith("DUMP,"):
            parts = line.split(",")
            if len(parts) == 5:
                _, _, ts_s, dev_time, name = parts
                try:
                    ts = int(ts_s)
                except ValueError:
                    continue
                if ts > 0 and ts not in seen:
                    seen.add(ts)
                    mac_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S") + " (补账)"
                    result = "DENY" if name == "DENIED" else "OK"
                    row = [mac_time, ts, dev_time, name, UID_BY_NAME.get(name, ""), result]
                    append_row(row)
                    push_to_sheet(row)
                    added += 1
    return added


def find_port():
    ports = glob.glob("/dev/cu.usbmodem*")
    return ports[0] if ports else None


def run():
    while True:
        if os.path.exists(PAUSE_FLAG):
            time.sleep(2)
            continue
        port = find_port()
        if not port:
            time.sleep(5)
            continue
        try:
            with serial.Serial(port, 9600, timeout=2) as ser:
                time.sleep(2.5)          # 打开串口会触发Uno复位,等它开机
                ser.reset_input_buffer()
                sync_time(ser)           # 1) 自动对表
                reconcile(ser)           # 2) 自动补离线账
                while True:              # 3) 实时监听
                    if os.path.exists(PAUSE_FLAG):
                        break
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").strip()
                    if line.startswith("LOG,"):
                        handle_log_line(line)
                    elif "CLOCK,WAITING" in line:
                        sync_time(ser)   # Uno重启后广播求对表,立刻喂时间
        except (serial.SerialException, OSError):
            time.sleep(5)


if __name__ == "__main__":
    run()
