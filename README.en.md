# RFID Access Logger (Arduino Uno + ESP32 Bridge)

> 中文原版(Chinese original): [README.md](README.md)

Swipe an RFID card → LCD shows welcome/denied + time → the record is **simultaneously** saved to the Uno's EEPROM (survives power loss)
→ and uploaded in real time to **Google Sheet** (via a Python logger on your Mac, or an ESP32 WiFi bridge — pick one, or use both).

A **complete step-by-step guide** from parts to finished build — get each step working before moving on to the next. All the pitfalls I hit along the way are written up in section 8.

```
                 ┌──────────────┐
   RFID card ───▶│ RC522 reader │
                 └──────┬───────┘
                        │ SPI (throttled to 250kHz)
                 ┌──────▼───────────────┐ USB serial             ┌───────────────┐
                 │ Arduino Uno          │◀──────────────────────▶│ Mac logger.py │──▶ CSV + Google Sheet
                 │ LCD1602 display      │                        └───────────────┘
                 │ EEPROM: 200 entries  │ SoftwareSerial         ┌────────────────────┐
                 │ soft clock (millis)  │◀──────────────────────▶│ ESP32 WiFi bridge  │──▶ Google Sheet (no Mac needed)
                 └──────────────────────┘A0/A1 + voltage divider │ NTP time sync      │
                                                                 └────────────────────┘
```

**Two paths:**
- **Path A (this guide's main track)**: Uno + LCD + RC522, with the Mac or ESP32 handling networking. Good if you already have an Uno kit on hand.
- **Path B (ESP32-only, single board)**: one ESP32 handles card reading + servo lock + web panel — see [GUIDE.md](GUIDE.md), firmware in `firmware/stage1~5`.

> 🔑 **Secrets management**: all private values (webhook URLs, card numbers, WiFi password) live in gitignored
> `secrets.h` / `config.json` files — the source only commits `*.example` templates. Just copy the template as you go through the guide and fill in your own values.

---

## 1. Parts List (Path A)

| Part | Qty | Notes |
|---|---|---|
| Arduino Uno | 1 | Main controller |
| LCD1602 display | 1 | 4-bit parallel interface, no I2C backpack needed |
| RC522 RFID reader + card/key fob | 1 set | **Must be wired to 3.3V** — 5V will fry it |
| ESP32 dev board (optional) | 1 | WiFi bridge: upload in real time without a computer plugged in |
| 220Ω resistor | 3 | For the ESP32 bridge's 5V→3.3V voltage divider (1 + 2 in series) |
| Breadboard + jumper wires | as needed | RC522 boards mostly ship **without headers soldered** — needs a soldering iron |
| USB cable | 1 | Note: many USB cables are charge-only — if the board isn't detected, swap the cable first |

## 2. Software Setup

**Option 1: Arduino IDE (recommended for beginners)**
Install via Library Manager: `MFRC522` (GithubCommunity version), `LiquidCrystal` (built-in), `RTClib` (Adafruit — only used for its DateTime conversion).

**Option 2: arduino-cli (for the command-line crowd)**
```bash
brew install arduino-cli
arduino-cli core install arduino:avr        # Uno core
arduino-cli core install esp32:esp32        # ESP32 core (only needed for step 7)
arduino-cli lib install MFRC522 RTClib
```

## 3. Wiring (Uno host)

**LCD1602 (4-bit parallel):**

| LCD pin | Connects to Uno | Notes |
|---|---|---|
| RS | D8 | |
| E | D7 | |
| D4~D7 | D5, D4, D3, D2 | |
| V0 (contrast) | D6 | Use PWM for contrast, **no potentiometer needed** (see section 8, pitfall #3) |
| VSS/RW | GND | Tie RW directly to ground (write-only, no reads) |
| VDD/A (backlight +) | 5V | A 220Ω resistor in series with the backlight is more stable |
| K (backlight -) | GND | |

**RC522 (SPI):**

| RC522 pin | Connects to Uno | Notes |
|---|---|---|
| SDA(SS) | D10 | |
| SCK | D13 | |
| MOSI | D11 | |
| MISO | D12 | |
| RST | D9 | |
| **VCC** | **3.3V** | ⚠️ 5V will fry it |
| GND | GND | |

**No RTC module needed.** This project uses a "soft clock": the Mac or ESP32 automatically feeds accurate time to the Uno (see steps 6 and 7).

## 4. Stage One: Get Card Reading + Display Working

1. Flash `firmware/uno_lcd_rfid/` (basic version: LCD + card-read display).
   ```bash
   cd firmware
   arduino-cli compile --fqbn arduino:avr:uno uno_lcd_rfid
   arduino-cli upload -p /dev/cu.usbmodem101 --fqbn arduino:avr:uno uno_lcd_rfid
   ```
2. Open the serial monitor (9600 baud), tap a card, and **write down the printed card ID (8-digit hex)**.
3. Configure the whitelist:
   ```bash
   cd uno_access_log
   cp secrets.h.example secrets.h    # then replace the example card ID in secrets.h with the one you wrote down
   ```
4. Flash `firmware/uno_access_log/` (the production version). Known cards show `Welcome <name>`, unknown cards show `Access denied`, and every tap is saved to EEPROM (survives power loss, ring-buffer overwrite, 200-entry capacity).

**Serial commands** (9600 baud, for humans and host software):

| Command | Action |
|---|---|
| `d` | Export all EEPROM records as CSV |
| `c` | Clear EEPROM records |
| `t` | Query current time |
| `T<unix-seconds>` | Sync the clock (e.g. `T1783991311`). logger.py and the ESP32 send this automatically — you generally don't need to type it by hand |

> It's normal for the second line of the display to show `no clock` at this point — after a restart the Uno broadcasts `CLOCK,WAITING` every 3 seconds asking for a time sync; hook up either step 6 or step 7 and the time will appear automatically.

## 5. Google Sheet Receiver (5 minutes)

1. Create a new Google Sheet, and fill in the header row: `mac_time | device_unix | device_time | name | uid | result`.
2. From the menu, go to **Extensions → Apps Script**, and paste in the contents of [google-apps-script/Code.gs](google-apps-script/Code.gs) (make sure `SHEET_NAME` matches your tab's name).
3. Click **Deploy → New deployment → Type: Web app**:
   - Execute as: **Me**
   - Who has access: **Anyone**
4. Copy the deployed URL (something like `https://script.google.com/macros/s/xxxxx/exec`), and paste it into:
   - `config.json` (created in step 6)
   - `firmware/esp32_bridge/secrets.h` (created in step 7)

> ⚠️ This URL is effectively your "write password" — anyone who knows it can push data into your sheet. It should only ever appear in gitignored files.

## 6. Path A-1: Using the Mac as a Logger (logger.py)

```bash
cp config.example.json config.json   # fill in your webhook and "name -> card ID" mapping
python3 -m venv .venv && .venv/bin/pip install pyserial
.venv/bin/python logger.py &
```

logger.py automatically handles four things:
1. **Finds the Uno's serial port** and listens on it — if unplugged, it automatically waits to reconnect;
2. **Syncs the clock on every connect** (writes the Mac's time to the Uno via the `T` command);
3. **Automatically backfills**: cards tapped while the Uno was offline (e.g. unplugged and running off a power bank) stay in EEPROM — the moment it's plugged back into the Mac, those entries are auto-exported, de-duplicated by timestamp, and backfilled into the CSV and Google Sheet (backfilled rows are marked "(backfill)");
4. Every live record is appended to the local `access_log.csv` **and** POSTed to the Google Sheet.

> When you need to reflash firmware, first run `touch /tmp/accesslogger.pause` to make the logger release the serial port, then `rm` that file once flashing is done.

## 7. Path A-2: ESP32 WiFi Bridge (real-time upload without a Mac plugged in)

**Wiring (watch the voltage divider! The Uno runs 5V signals, the ESP32 only tolerates 3.3V):**

```
Uno A1(TX) ──[220Ω]──┬──[440Ω (two 220Ω in series)]── GND
                      └──→ ESP32 GPIO16 (RX2)
ESP32 GPIO17 (TX2) ──direct──→ Uno A0(RX)      (3.3V signal, the Uno can read it directly)
Uno 5V → ESP32 VIN, share GND (required!)
```

**Flashing:**
1. Configure:
   ```bash
   cd firmware/esp32_bridge
   cp secrets.h.example secrets.h    # fill in your WiFi name/password + the webhook URL from step 5
   ```
2. Connect the ESP32 to your computer via USB:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 esp32_bridge
   arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32:UploadSpeed=115200 esp32_bridge
   ```
   (Clone boards' USB chips fail to flash at the default 921600 baud — **dropping to 115200** fixes it, see section 8, pitfall #6)
3. Once flashing is done, unplug it from the computer and wire it to the Uno as shown above.

**How it works:** the ESP32 connects to WiFi → gets accurate time from NTP → feeds it to the Uno every hour (if the Uno restarts and broadcasts a sync request, it's fed instantly); on every card tap, the Uno sends the log to both USB and the ESP32, and the ESP32 POSTs it straight to the Google Sheet; it auto-reconnects on WiFi drop and keeps a 20-entry in-memory retry queue. **The onboard blue LED is a status light: solid on means WiFi + NTP are both healthy.**

At this point the whole system can run standalone off a power bank: tap card → screen feedback → EEPROM backup → the record shows up on Google Sheet within seconds.

## 8. Lessons from the Trenches (each one genuinely cost hours of my life)

1. **Clone RC522 boards only work at 250kHz SPI.** At the default 4MHz, self-test passes fine but the card never reads. Fix: `#define MFRC522_SPICLOCK (250000u)` must come **before** `#include <MFRC522.h>`. This is the deepest pitfall in this whole project.
2. **RC522 header pins ship unsoldered, and a loose fit is intermittent.** Symptom: worked yesterday, "dropped" today. You must solder them, or buy a pre-soldered version.
3. **Skip the potentiometer for LCD contrast:** wire V0 to a PWM pin and adjust directly with `analogWrite(V0_PIN, 40)` — saves a potentiometer. Characters turning into solid blocks means raise the value; invisible characters means lower it.
4. **Charge-only USB cables won't let the board be detected.** If nothing shows up in the serial port list, swap the cable first, then check the driver (CH340/CP210x).
5. **Opening the serial port resets the Uno.** Host software (logger.py) should wait ~2.5 seconds after opening the port before communicating, otherwise every command you send gets fed to the still-booting bootloader.
6. **Clone ESP32 boards get stuck at "Connecting..." or time out when flashing:** drop the baud rate to 115200; if that's still not enough, hold the BOOT button on the board while starting the flash.
7. **RTC modules (DS1302/DS3231) are extremely unreliable on a breadboard.** This project originally used a DS1302, and flaky contacts kept making it "lose its memory"; in the end I just removed the hardware entirely and switched to a soft clock + network time sync (NTP) — **one fewer part = one fewer failure point**.
8. **Cross-board communication needs a shared ground.** If the Uno and ESP32 are powered separately without a common GND, the serial output is pure garbage.
9. **5V→3.3V requires a voltage divider.** Wiring Uno TX straight into ESP32 RX might work short-term, but it'll fry the pin over time. It's just two or three resistors — don't skip it.

## 9. Security Notes

- Real card IDs, webhooks, and WiFi passwords should only live in `secrets.h` / `config.json` (already gitignored) — **before committing, make sure you haven't added them** (they shouldn't show up in `git status`).
- RFID card UIDs can be cloned with cheap devices, so this system is suited to **logging/attendance** use cases; if you actually want to control a door lock, switch to a card scheme that supports encrypted sectors.
- The Apps Script webhook URL is equivalent to write access — if it leaks, **redeploy** to get a new URL.

## 10. Repository Structure

```
README.md                  ← this guide (Path A: Uno main track)
GUIDE.md                   ← Path B: ESP32-only single-board guide (servo lock + web panel)
logger.py                  ← Mac logger: clock sync + backfill + CSV + Google Sheet
config.example.json        ← logger config template (copy to config.json and fill in real values)
google-apps-script/Code.gs ← Google Sheet receiver
firmware/
  uno_lcd_rfid/            ← Stage one: LCD + card read (get this working first)
  uno_access_log/          ← production firmware: whitelist + EEPROM + soft clock + dual serial
    secrets.h.example      ←   whitelist template (copy to secrets.h and fill in your card IDs)
  esp32_bridge/            ← ESP32 WiFi bridge: NTP time sync + direct-to-Google-Sheet upload
    secrets.h.example      ←   WiFi/webhook template (copy to secrets.h)
  stage1~5_*/              ← the five stages of Path B (ESP32-only)
  diag_*/                  ← small hardware diagnostic tools (pin probing, RC522 self-test, etc.)
```
