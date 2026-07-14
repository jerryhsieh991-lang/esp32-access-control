/*
  ESP32 WiFi+NTP 桥接板 —— 给 Arduino Uno 门禁系统当"联网小助手"
  --------------------------------------------------------------
  Uno 自己不能连WiFi,也没有靠谱的实时时钟,所以用这块ESP32:
    1) 连WiFi、连NTP服务器,把"现在几点"通过串口喂给Uno
    2) Uno刷卡产生的日志,通过串口转发给这块板子,再由它POST到
       Google Apps Script 的网页(Uno自己没法直接发HTTPS请求)

  接线(硬件说明):
    - ESP32 与 Uno 之间用 UART2 通信,9600波特:
        ESP32 RX2(GPIO16) <---- 分压电阻 ---- Uno A1 (当TX用)
          Uno输出是5V,ESP32的IO口只能耐受3.3V,所以Uno->ESP32这一路
          必须经过分压电阻(比如 1kΩ+2kΩ,或者现成的双向电平转换模块),
          不分压直接接,长期可能烧坏ESP32的GPIO16。
        ESP32 TX2(GPIO17) ----直接接----> Uno A0 (当RX用)
          ESP32是3.3V输出,Uno的输入脚在3.3V时也能识别成高电平,
          所以这一路不需要分压,直接接就行。
        代码里 Serial2.begin(9600, SERIAL_8N1, 16, 17);
    - 供电: ESP32的VIN接Uno的5V输出(或者ESP32自己用USB供电都行),
      两块板子必须共地(GND接GND),不然UART信号会乱。
    - USB Serial(Serial,115200波特)只用来在电脑上看调试信息,
      跟Uno通信完全走UART2(Serial2),两路互不干扰。
    - 板载LED(GPIO2): WiFi和NTP都正常时常亮,只要有一个没好就熄灭,
      一眼就能看出这块板子是否"联网正常"。

  行为说明:
    - WiFi断线不会卡死程序,每15秒自动重连一次,重连期间其它功能
      (读Uno的日志、维护重试队列)照常运行。
    - NTP首次同步成功后,立刻给Uno发一次当前时间,之后每60分钟
      自动补发一次,防止Uno自己的计时慢慢跑偏。
    - 如果Uno重启了(它开机时会一直发"CLOCK,WAITING"等着要时间),
      ESP32只要收到含有"CLOCK,WAITING"的行,就会立刻重发一次时间,
      不用等到下一个60分钟。
    - Uno发来的 "LOG,时间戳,时间字符串,姓名,卡号,结果" 会被解析成JSON
      转发到网页后台;网络不通或者发送失败时,先存进内存里的环形队列
      (最多存20条,超过就丢最老的一条),每60秒等WiFi正常时自动重试。
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ================== 改成你家WiFi ==================
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"
// ===================================================

// 洛杉矶时区(带夏令时自动切换),NTP服务器用两个,一个挂了还有备用的
#define TZ_INFO "PST8PDT,M3.2.0,M11.1.0"
const char *NTP_SERVER1 = "pool.ntp.org";
const char *NTP_SERVER2 = "time.google.com";

// 门禁日志要POST到的网页(Google Apps Script)
const char *WEBHOOK_URL =
    "https://script.google.com/macros/s/YOUR_DEPLOYMENT_ID/exec";

#define LED_PIN 2

// ---------- 各种时间间隔 ----------
const unsigned long WIFI_RETRY_MS = 15UL * 1000UL;         // WiFi断线后每15秒重试
const unsigned long TIME_SEND_INTERVAL_MS = 60UL * 60UL * 1000UL; // 每60分钟补发一次时间
const unsigned long QUEUE_FLUSH_INTERVAL_MS = 60UL * 1000UL;      // 每60秒清一次重试队列

// ---------- 重试队列(环形缓冲区,最多存20条待发的JSON) ----------
#define QUEUE_CAP 20
String jsonQueue[QUEUE_CAP];
int qHead = 0;    // 最老的一条在哪个位置
int qCount = 0;   // 队列里现在有几条

// ---------- 状态变量 ----------
unsigned long lastWifiAttempt = 0;
unsigned long lastTimeSendMillis = 0;
unsigned long lastQueueFlush = 0;
bool wifiWasConnected = false;
bool ntpConfigured = false;
bool ntpSyncedPrev = false;
String uartBuf = "";   // 从Uno收字符,拼成一整行

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17); // RX2=16 TX2=17,跟Uno通信
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  WiFi.mode(WIFI_STA);
  Serial.println(F("[BOOT] ESP32桥接板启动"));
}

void loop() {
  maintainWiFi();

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  // 刚连上WiFi:启动NTP同步(只需要调用一次)
  if (wifiConnected && !wifiWasConnected) {
    Serial.print(F("[WiFi] 已连接: "));
    Serial.println(WiFi.localIP());
    if (!ntpConfigured) {
      configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
      ntpConfigured = true;
      Serial.println(F("[NTP] 已启动同步"));
    }
  }
  if (!wifiConnected && wifiWasConnected) {
    Serial.println(F("[WiFi] 断线了,进入重试模式"));
  }
  wifiWasConnected = wifiConnected;

  bool ntpSyncedNow = isNtpSynced();

  // 首次同步成功:立刻给Uno发一次时间
  if (ntpSyncedNow && !ntpSyncedPrev) {
    Serial.println(F("[NTP] 首次同步成功"));
    sendTimeToUno();
  }
  ntpSyncedPrev = ntpSyncedNow;

  // 每60分钟补发一次时间,防止Uno那边慢慢跑偏
  if (ntpSyncedNow && (millis() - lastTimeSendMillis >= TIME_SEND_INTERVAL_MS)) {
    sendTimeToUno();
  }

  handleUnoSerial();

  // 每60秒,WiFi正常的话就把没发出去的日志再试一次
  if (wifiConnected && (millis() - lastQueueFlush >= QUEUE_FLUSH_INTERVAL_MS)) {
    lastQueueFlush = millis();
    flushQueue();
  }

  digitalWrite(LED_PIN, (wifiConnected && ntpSyncedNow) ? HIGH : LOW);
}

// ============================================================
// WiFi: 非阻塞重连,断线不影响其它功能
// ============================================================
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (lastWifiAttempt == 0 || now - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = now;
    Serial.println(F("[WiFi] 尝试连接..."));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// 判断NTP是不是已经同步过:系统时间如果还停在1970年附近,说明没同步
bool isNtpSynced() {
  time_t now = time(nullptr);
  return now > 1700000000; // 随便选一个远大于"没同步"的时间点(2023年之后)
}

// ============================================================
// 把当前时间发给Uno: "T<epoch>\n"
// epoch是"本地墙上时间"按UTC方式算出来的时间戳,
// 也就是 gmtime(epoch) 出来的年月日时分秒,要跟洛杉矶当地时间一致
// (Uno那边不知道时区,只想要一串能直接拆成年月日时分秒的数字)
// ============================================================
void sendTimeToUno() {
  struct tm t;
  if (!getLocalTime(&t, 200)) {
    Serial.println(F("[TIME] 还没拿到本地时间,先不发"));
    return;
  }
  time_t wallEpoch = civilToEpoch(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                                   t.tm_hour, t.tm_min, t.tm_sec);
  Serial2.print('T');
  Serial2.print((unsigned long)wallEpoch);
  Serial2.print('\n');
  lastTimeSendMillis = millis();
  Serial.print(F("[TIME] 已发送给Uno: T"));
  Serial.println((unsigned long)wallEpoch);
}

// 把"年月日"转成从1970-01-01开始的天数(Howard Hinnant的经典算法,亲测好用)
long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

// 年月日时分秒 -> 时间戳(把输入的这组数字当成UTC来算,不管它实际是哪个时区)
time_t civilToEpoch(int year, int mon, int day, int hour, int minute, int sec) {
  long days = daysFromCivil(year, mon, day);
  long secs = days * 86400L + (long)hour * 3600L + (long)minute * 60L + sec;
  return (time_t)secs;
}

// ============================================================
// 读Uno发来的每一行,拼好之后交给processUnoLine处理
// ============================================================
void handleUnoSerial() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      uartBuf.trim();
      if (uartBuf.length() > 0) processUnoLine(uartBuf);
      uartBuf = "";
    } else if (c != '\r') {
      uartBuf += c;
      if (uartBuf.length() > 200) uartBuf = ""; // 异常数据太长,直接丢掉重新开始
    }
  }
}

void processUnoLine(String line) {
  Serial.print(F("[UNO] "));
  Serial.println(line);

  if (line.indexOf("CLOCK,WAITING") >= 0) {
    // Uno重启后一直在等时间,收到就立刻补发一次
    sendTimeToUno();
    return;
  }
  if (line.startsWith("LOG,")) {
    handleLogLine(line);
  }
}

// ============================================================
// 解析 LOG,<device_unix>,<device_time>,<name>,<uid>,<result>
// 转成JSON,POST到网页后台;失败就存进重试队列
// ============================================================
void handleLogLine(String line) {
  String f[6]; // f[0]="LOG", f[1..5]=数据字段
  if (!splitFields(line, f, 6)) {
    Serial.println(F("[LOG] 格式不对,丢弃这一行"));
    return;
  }

  String payload = buildJson(f[1], f[2], f[3], f[4], f[5]);
  bool ok = postJson(payload);
  if (ok) {
    Serial.println(F("[LOG] 转发成功"));
  } else {
    Serial.println(F("[LOG] 转发失败,存入重试队列"));
    enqueuePayload(payload);
  }
  Serial.print(F("[QUEUE] 当前队列条数: "));
  Serial.println(qCount);
}

// 按逗号切成maxFields份;最后一份不管里面还有没有逗号,原样保留
bool splitFields(String line, String *out, int maxFields) {
  int start = 0;
  for (int i = 0; i < maxFields; i++) {
    if (i == maxFields - 1) {
      out[i] = line.substring(start);
    } else {
      int idx = line.indexOf(',', start);
      if (idx < 0) return false; // 逗号不够,格式不对
      out[i] = line.substring(start, idx);
      start = idx + 1;
    }
  }
  return true;
}

String escapeJson(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  return s;
}

String buildJson(String deviceUnix, String deviceTime, String name, String uid, String result) {
  String j = "{";
  j += "\"mac_time\":\"esp32\",";
  j += "\"device_unix\":\"" + escapeJson(deviceUnix) + "\",";
  j += "\"device_time\":\"" + escapeJson(deviceTime) + "\",";
  j += "\"name\":\"" + escapeJson(name) + "\",";
  j += "\"uid\":\"" + escapeJson(uid) + "\",";
  j += "\"result\":\"" + escapeJson(result) + "\"";
  j += "}";
  return j;
}

// 真正发HTTP POST;302和200都算成功(Apps Script成功时会跳转)
bool postJson(String payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // 教学用,不校验证书;正式项目建议换成校验证书的写法
  HTTPClient https;
  if (!https.begin(client, WEBHOOK_URL)) return false;
  https.addHeader("Content-Type", "application/json");

  int code = https.POST(payload);
  https.end();

  Serial.print(F("[HTTP] 返回码: "));
  Serial.println(code);
  return (code == 200 || code == 302);
}

// ============================================================
// 重试队列:环形缓冲区,最多20条,满了就丢最老的那条
// ============================================================
void enqueuePayload(String payload) {
  if (qCount >= QUEUE_CAP) {
    qHead = (qHead + 1) % QUEUE_CAP; // 丢弃最老的一条
    qCount--;
  }
  int tail = (qHead + qCount) % QUEUE_CAP;
  jsonQueue[tail] = payload;
  qCount++;
}

// 从最老的一条开始依次重发,遇到失败就停下,留到下次再试(保持顺序)
void flushQueue() {
  while (qCount > 0) {
    bool ok = postJson(jsonQueue[qHead]);
    if (!ok) break;
    qHead = (qHead + 1) % QUEUE_CAP;
    qCount--;
  }
  if (qCount > 0) {
    Serial.print(F("[QUEUE] 还剩"));
    Serial.print(qCount);
    Serial.println(F("条没发出去,下次再试"));
  }
}
