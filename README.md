# RFID 门禁记录系统(Arduino Uno + ESP32 桥)

刷 RFID 卡 → LCD 显示欢迎/拒绝 + 时间 → 记录**同时**存进 Uno 的 EEPROM(断电不丢)
→ 并实时上传到 **Google Sheet**(通过 Mac 上的 Python 记录器,或 ESP32 WiFi 桥,二选一/都可以)。

从零件到成品的**完整分步教程**,每一步先跑通再进行下一步。所有踩过的坑都写在第 8 节。

```
                 ┌──────────────┐
   RFID卡 ──────▶│ RC522 读卡器  │
                 └──────┬───────┘
                        │ SPI(降速250kHz)
                 ┌──────▼───────┐        USB串口          ┌─────────────┐
                 │ Arduino Uno  │◀──────────────────────▶│ Mac logger.py│──▶ CSV + Google Sheet
                 │  LCD1602 显示 │                         └─────────────┘
                 │  EEPROM 存200条│       SoftwareSerial   ┌─────────────┐
                 │  软时钟(millis)│◀──────────────────────▶│ ESP32 WiFi桥 │──▶ Google Sheet(不用Mac)
                 └──────────────┘        A0/A1 + 分压      │  NTP 授时    │
                 └─────────────┘
```

**两条路线:**
- **路线 A(本教程主线)**:Uno + LCD + RC522,Mac 或 ESP32 负责联网。适合手上是 Uno 套件的人。
- **路线 B(纯 ESP32 单板)**:一块 ESP32 搞定读卡+舵机开锁+网页面板,见 [GUIDE.md](GUIDE.md),固件在 `firmware/stage1~5`。

> 🔑 **密钥管理**:所有私密值(webhook 地址、卡号、WiFi 密码)都放在被 gitignore 的
> `secrets.h` / `config.json` 里,源码只提交 `*.example` 模板。照教程复制模板填入自己的值即可。

---

## 1. 零件清单(路线 A)

| 零件 | 数量 | 说明 |
|---|---|---|
| Arduino Uno | 1 | 主控 |
| LCD1602 液晶屏 | 1 | 4 位并口驱动,不需要 I2C 背包 |
| RC522 RFID 读卡器 + 卡/钥匙扣 | 1 套 | **必须接 3.3V**,接 5V 会烧 |
| ESP32 开发板(可选) | 1 | WiFi 桥:不插电脑也能实时上传 |
| 220Ω 电阻 | 3 | ESP32 桥的 5V→3.3V 分压用(1 个 + 2 个串联) |
| 面包板 + 杜邦线 | 若干 | RC522 出厂大多**不焊排针**,需要电烙铁焊上 |
| USB 数据线 | 1 | 注意:很多 USB 线是纯充电线,认不出板子先换线 |

## 2. 软件准备

**方式一:Arduino IDE(新手推荐)**
库管理器安装:`MFRC522`(GithubCommunity 版)、`LiquidCrystal`(内置)、`RTClib`(Adafruit,只用它的 DateTime 换算)。

**方式二:arduino-cli(命令行党)**
```bash
brew install arduino-cli
arduino-cli core install arduino:avr        # Uno 核心
arduino-cli core install esp32:esp32        # ESP32 核心(做第7步才需要)
arduino-cli lib install MFRC522 RTClib
```

## 3. 接线(Uno 主机)

**LCD1602(4 位并口):**

| LCD 引脚 | 接 Uno | 说明 |
|---|---|---|
| RS | D8 | |
| E | D7 | |
| D4~D7 | D5, D4, D3, D2 | |
| V0(对比度) | D6 | 用 PWM 当对比度,**不用电位器**(见第 8 节坑 #3) |
| VSS/RW | GND | RW 直接接地(只写不读) |
| VDD/A(背光+) | 5V | 背光串 220Ω 更稳 |
| K(背光-) | GND | |

**RC522(SPI):**

| RC522 引脚 | 接 Uno | 说明 |
|---|---|---|
| SDA(SS) | D10 | |
| SCK | D13 | |
| MOSI | D11 | |
| MISO | D12 | |
| RST | D9 | |
| **VCC** | **3.3V** | ⚠️ 接 5V 会烧 |
| GND | GND | |

**不需要 RTC 时钟模块。** 本项目用"软时钟":Mac 或 ESP32 会自动把精确时间喂给 Uno(见第 6、7 步)。

## 4. 第一阶段:跑通读卡 + 显示

1. 烧录 `firmware/uno_lcd_rfid/`(基础版:LCD + 读卡显示)。
   ```bash
   cd firmware
   arduino-cli compile --fqbn arduino:avr:uno uno_lcd_rfid
   arduino-cli upload -p /dev/cu.usbmodem101 --fqbn arduino:avr:uno uno_lcd_rfid
   ```
2. 打开串口监视器(9600 波特),刷卡,**把打印出来的卡号(8 位十六进制)抄下来**。
3. 配置白名单:
   ```bash
   cd uno_access_log
   cp secrets.h.example secrets.h    # 然后把 secrets.h 里的示例卡号换成你抄下来的
   ```
4. 烧录 `firmware/uno_access_log/`(正式版)。刷已知卡显示 `Welcome 名字`,陌生卡显示 `Access denied`,每次刷卡都会存进 EEPROM(断电不丢,环形覆盖,容量 200 条)。

**串口命令**(9600 波特,给人和上位机用):

| 命令 | 作用 |
|---|---|
| `d` | 把 EEPROM 里全部记录导出成 CSV |
| `c` | 清空 EEPROM 记录 |
| `t` | 查询当前时间 |
| `T<unix秒>` | 对表(例:`T1783991311`)。logger.py 和 ESP32 会自动发,一般不用手动 |

> 此时屏幕第二行显示 `no clock` 是正常的——Uno 重启后会每 3 秒广播 `CLOCK,WAITING` 求对表,接上第 6 或第 7 步的任意一个,时间自动出现。

## 5. Google Sheet 接收端(5 分钟)

1. 新建一个 Google Sheet,第一行填表头:`mac_time | device_unix | device_time | name | uid | result`。
2. 菜单 **扩展程序 → Apps Script**,把 [google-apps-script/Code.gs](google-apps-script/Code.gs) 的内容粘进去(注意 `SHEET_NAME` 和你的标签页名一致)。
3. 点 **部署 → 新建部署 → 类型:Web 应用**:
   - 执行身份:**我**
   - 谁可以访问:**任何人**
4. 复制部署后的 URL(形如 `https://script.google.com/macros/s/xxxxx/exec`),填到:
   - `config.json`(第 6 步创建)
   - `firmware/esp32_bridge/secrets.h`(第 7 步创建)

> ⚠️ 这个 URL 就是你的"写入密码"——知道 URL 的人就能往你表格灌数据。它只该出现在被 gitignore 的文件里。

## 6. 路线 A-1:Mac 当记录器(logger.py)

```bash
cp config.example.json config.json   # 填入你的 webhook 和"名字→卡号"对照表
python3 -m venv .venv && .venv/bin/pip install pyserial
.venv/bin/python logger.py &
```

logger.py 会自动完成四件事:
1. **找到 Uno 串口**并监听,拔掉自动等待重连;
2. **每次连上自动对表**(把 Mac 的时间用 `T` 命令写给 Uno);
3. **自动补账**:Uno 离线期间(比如拔下来插充电宝)刷的卡存在 EEPROM 里,插回 Mac 的瞬间自动导出、按时间戳去重、补进 CSV 和 Google Sheet(补的行标"(补账)");
4. 每条实时记录追加到本地 `access_log.csv` **并** POST 到 Google Sheet。

> 需要重新烧录固件时,先 `touch /tmp/accesslogger.pause` 让 logger 释放串口,烧完 `rm` 掉该文件。

## 7. 路线 A-2:ESP32 WiFi 桥(不插 Mac 也能实时上传)

**接线(注意分压!Uno 是 5V 信号,ESP32 只耐 3.3V):**

```
Uno A1(TX) ──[220Ω]──┬──[440Ω(两个220串联)]── GND
                      └──→ ESP32 GPIO16 (RX2)
ESP32 GPIO17 (TX2) ──直连──→ Uno A0(RX)      (3.3V 信号 Uno 能直接读)
Uno 5V → ESP32 VIN,GND 共地(必须!)
```

**烧录:**
1. 配置:
   ```bash
   cd firmware/esp32_bridge
   cp secrets.h.example secrets.h    # 填你家 WiFi 名/密码 + 第5步的 webhook URL
   ```
2. ESP32 用 USB 线接电脑:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 esp32_bridge
   arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32:UploadSpeed=115200 esp32_bridge
   ```
   (仿板 USB 芯片在默认 921600 波特下烧录会失败,**降到 115200** 就好,见第 8 节坑 #6)
3. 烧完拔掉电脑,按上面接线接到 Uno。

**工作方式:** ESP32 连 WiFi → NTP 拿精确时间 → 每小时喂给 Uno(Uno 重启会主动广播求对表,秒喂);Uno 每次刷卡把日志同时发 USB 和 ESP32,ESP32 直接 POST 进 Google Sheet;断网自动重连 + 20 条内存重试队列。**板载蓝灯 = 状态灯:常亮说明 WiFi+NTP 全正常。**

到这一步,整套系统插充电宝就能独立工作:刷卡 → 屏幕反馈 → EEPROM 留底 → Google Sheet 秒现记录。

## 8. 踩坑实录(每个都真实浪费过几小时)

1. **RC522 仿板只能跑 250kHz SPI。** 默认 4MHz 下自检全过但永远读不到卡。解法:`#define MFRC522_SPICLOCK (250000u)` 必须在 `#include <MFRC522.h>` **之前**。这是本项目最深的坑。
2. **RC522 排针出厂不焊,虚插时好时坏。** 症状:昨天能用今天"掉线"。必须焊接,或买已焊版。
3. **LCD 对比度不用电位器:** V0 接一个 PWM 脚,`analogWrite(V0_PIN, 40)` 直接调,省一个电位器。字变成空心方块=调大数值,看不见字=调小。
4. **纯充电 USB 线认不出板子。** 串口列表里没有设备先换线,再查驱动(CH340/CP210x)。
5. **打开串口 = Uno 复位。** 上位机(logger.py)打开串口后要等 ~2.5 秒再通信,否则发的命令全喂给了正在启动的 bootloader。
6. **ESP32 仿板烧录卡 "Connecting..." 或超时:** 波特率降到 115200;还不行就按住板上 BOOT 键再点烧录。
7. **RTC 模块(DS1302/DS3231)在面包板上极不可靠。** 本项目最初用 DS1302,接触不良反复"失忆",最后干脆删掉硬件,改用软时钟 + 网络授时(NTP)——**少一个零件 = 少一个故障点**。
8. **跨板通信必须共地。** Uno 和 ESP32 各自供电但 GND 不连,串口读出来全是乱码。
9. **5V→3.3V 必须分压。** Uno TX 直怼 ESP32 RX 短期能用,长期烧脚。两三个电阻的事,别省。

## 9. 安全须知

- 真实卡号、webhook、WiFi 密码只存在于 `secrets.h` / `config.json`(已 gitignore),**提交代码前确认没把它们加进去**(`git status` 里不该出现)。
- RFID 卡的 UID 可以被廉价设备克隆,这套系统适合**记录/考勤**场景;真拿来控制门锁,请换支持加密扇区的卡片方案。
- Apps Script 的 webhook URL 等于写入权限,泄露了就**重新部署**换一个 URL。

## 10. 仓库结构

```
README.md                  ← 本教程(路线 A:Uno 主线)
GUIDE.md                   ← 路线 B:纯 ESP32 单板教程(舵机开锁+网页面板)
logger.py                  ← Mac 记录器:对表 + 补账 + CSV + Google Sheet
config.example.json        ← logger 配置模板(复制为 config.json 填真实值)
google-apps-script/Code.gs ← Google Sheet 接收端
firmware/
  uno_lcd_rfid/            ← 第一阶段:LCD + 读卡(先跑通这个)
  uno_access_log/          ← 正式固件:白名单 + EEPROM + 软时钟 + 双串口
    secrets.h.example      ←   白名单模板(复制为 secrets.h 填你的卡号)
  esp32_bridge/            ← ESP32 WiFi 桥:NTP 授时 + 直传 Google Sheet
    secrets.h.example      ←   WiFi/webhook 模板(复制为 secrets.h)
  stage1~5_*/              ← 路线 B 的五个阶段(纯 ESP32)
  diag_*/                  ← 硬件诊断小工具(引脚探测、RC522 自检等)
```
