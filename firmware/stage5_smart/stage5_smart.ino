/*
  阶段 5 — 智能升级版(WiFi + NTP对时 + 白名单持久化 + 主卡录卡 + 防暴力破解 + 网页看板)
  ------------------------------------------------------------------------------
  在阶段4"读卡+白名单+舵机+蜂鸣器"的基础上,升级成更像样的门禁:

    1. WiFi + NTP 自动对时:开机连WiFi(最多等10秒),连上就用网络时间戳事件;
       连不上也没关系,门禁核心逻辑完全不依赖WiFi,直接离线运行。
    2. 白名单存进 Preferences(NVS 闪存),断电不丢,不用每次改代码重新烧录。
    3. 主卡录卡:刷一下"主卡"进入15秒录卡模式(双灯闪烁),这15秒内刷的
       第一张陌生卡会被自动加入白名单并存进闪存。主卡本身不能开门,更安全。
    4. 防暴力破解:连续刷5次陌生卡会触发30秒锁定,锁定期间红灯常亮、
       每5秒响一次长报警,所有卡(包括白名单卡)一律无效。
    5. 内存里保留最近20条刷卡记录(时间/卡号/结果),串口打印 + 网页展示。
    6. 网页看板:WiFi连上后,浏览器访问ESP32的IP就能看到当前状态、
       白名单卡数、最近20条记录,还有"远程开门"和"解除锁定"两个按钮。

  完整接线(和阶段4完全一样,这个版本不需要DS3231 RTC模块了):
    RC522  SDA(SS) -> GPIO5    RC522 SCK  -> GPIO18
    RC522  MOSI     -> GPIO23  RC522 MISO -> GPIO19
    RC522  RST      -> GPIO27  RC522 VCC  -> 3.3V(不要接5V!) RC522 GND -> GND
    舵机   信号(橙) -> GPIO13   舵机 VCC(红) -> 5V(VIN)  舵机 GND(棕) -> GND
    绿LED  +(经220Ω) -> GPIO32  负极 -> GND
    红LED  +(经220Ω) -> GPIO33  负极 -> GND
    有源蜂鸣器 + -> GPIO25      - -> GND

  *** 使用前必须改的地方 ***
    1. 下面 WIFI_SSID / WIFI_PASS 改成你家WiFi(改不了也能用,只是没有网页和准确时间)
    2. MASTER_UID 换成你想当"主卡"的卡号(阶段1刷卡能读出来)
    3. 首次开机白名单会自动写入占位卡号 a1b2c3d4,记得用主卡+陌生卡的方式
       录入你自己的真实卡号,或者直接改 loadWhitelist() 里的默认值
*/

#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// ---------- WiFi 配置(改成你家WiFi) ----------
#define WIFI_SSID "your-wifi-name"       // 改成你家WiFi
#define WIFI_PASS "your-wifi-password"   // 改成你家WiFi密码
#define WIFI_TIMEOUT_MS 10000            // 开机最多等10秒,超时就离线运行

// ---------- 主卡(用于进入录卡模式,换成你想当主卡的卡号) ----------
#define MASTER_UID "deadbeef"   // 换成你想当主卡的卡号

// ---------- 引脚定义(与阶段4完全一致) ----------
#define SS_PIN 5
#define RST_PIN 27
#define GREEN_PIN 32
#define RED_PIN 33
#define BUZZ_PIN 25
#define SERVO_PIN 13

// ---------- 参数 ----------
#define LOCKED_ANGLE 0        // 0°  = 锁
#define OPEN_ANGLE 90         // 90° = 开
#define UNLOCK_MS 3000        // 开锁保持时间
#define BEEP_MS 120           // 拒绝时蜂鸣器单次响/停时长

#define MAX_WL 20              // 白名单最多存多少张卡
#define LOG_SIZE 20             // 最近事件记录条数

#define ENROLL_MS 15000         // 录卡模式持续时间
#define BLINK_MS 300            // 录卡模式下双灯闪烁间隔

#define DENY_LIMIT 5            // 连续拒绝多少次触发锁定
#define LOCKOUT_MS 30000        // 锁定持续时间
#define ALARM_INTERVAL_MS 5000  // 锁定期间报警蜂鸣间隔

MFRC522 rfid(SS_PIN, RST_PIN);
Servo lock;
Preferences prefs;
WebServer server(80);

// ---------- 白名单(启动时从 NVS 加载,运行中改动会立刻存回 NVS) ----------
String whitelist[MAX_WL];
int wlCount = 0;

// ---------- 最近事件环形缓冲区 ----------
String logTime[LOG_SIZE];
String logUid[LOG_SIZE];
String logResult[LOG_SIZE];
int logHead = 0;    // 下一条要写入的位置
int logCount = 0;   // 当前已有多少条(最多 LOG_SIZE)

// ---------- 状态变量 ----------
bool wifiConnected = false;

bool enrollMode = false;
unsigned long enrollStartMs = 0;
unsigned long lastBlinkMs = 0;
bool blinkState = false;

bool lockoutActive = false;
unsigned long lockoutStartMs = 0;
unsigned long lastAlarmMs = 0;
int denyCount = 0;

// =====================================================================
// 与阶段1完全相同的格式化函数(小写、每字节两位十六进制、补零),
// 全部阶段必须用同一个函数,否则白名单比对会对不上。
// =====================================================================
String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";   // 补前导零
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();
  return s;
}

// 读卡:没有新卡返回空字符串;读到卡则返回卡号,并在函数内部
// 完成 HaltA / StopCrypto1,调用方不用再管。
String readUID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return "";
  }
  String uid = uidToString(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// =====================================================================
// 时间(NTP)
// =====================================================================

// 返回 "YYYY-MM-DD HH:MM:SS";还没对上时就返回"未对时"
// getLocalTime 的超时时间给得很短(5ms),避免离线时每次调用都卡住
String nowString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5)) {
    return "未对时";
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// 开机时尝试连WiFi,最多等 WIFI_TIMEOUT_MS;连不上就离线运行,
// 门禁核心逻辑(读卡/白名单/开锁)完全不依赖这一步。
void connectWiFi() {
  Serial.print("正在连接WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print("WiFi已连接,IP地址: ");
    Serial.println(WiFi.localIP());
    configTzTime("CST-8", "pool.ntp.org");   // 中国时区,NTP对时
    Serial.println("正在后台同步网络时间...");
  } else {
    wifiConnected = false;
    Serial.println("WiFi连接失败(10秒超时),门禁将离线运行,时间显示为“未对时”");
  }
}

// =====================================================================
// 白名单(Preferences / NVS 持久化)
// =====================================================================

// 把逗号分隔的字符串拆进 whitelist[] 数组
void parseWhitelistCsv(String csv) {
  wlCount = 0;
  int start = 0;
  while (start < (int)csv.length() && wlCount < MAX_WL) {
    int comma = csv.indexOf(',', start);
    if (comma == -1) comma = csv.length();
    String piece = csv.substring(start, comma);
    piece.trim();
    if (piece.length() > 0) {
      whitelist[wlCount++] = piece;
    }
    start = comma + 1;
  }
}

// 把 whitelist[] 数组存回 NVS(逗号分隔)
void saveWhitelist() {
  String csv = "";
  for (int i = 0; i < wlCount; i++) {
    if (i > 0) csv += ",";
    csv += whitelist[i];
  }
  prefs.putString("wl", csv);
}

// 开机加载白名单;第一次开机(还没有 "wl" 这个key)就写入占位卡号
void loadWhitelist() {
  if (!prefs.isKey("wl")) {
    prefs.putString("wl", "a1b2c3d4");   // 换成你阶段1打印的卡号
    Serial.println("首次开机:白名单已写入占位卡号 a1b2c3d4,请尽快用主卡录入你的真实卡号!");
  }
  String csv = prefs.getString("wl", "");
  parseWhitelistCsv(csv);
}

bool isAllowed(String uid) {
  for (int i = 0; i < wlCount; i++) {
    if (whitelist[i] == uid) return true;
  }
  return false;
}

// 添加一张卡到白名单(自动去重),成功返回true
bool addCard(String uid) {
  if (isAllowed(uid)) return false;
  if (wlCount >= MAX_WL) {
    Serial.println("白名单已满,无法继续添加");
    return false;
  }
  whitelist[wlCount++] = uid;
  saveWhitelist();
  return true;
}

// =====================================================================
// 事件记录(内存环形缓冲区,最近 LOG_SIZE 条,不写闪存)
// =====================================================================
void addLog(String uid, String result) {
  String t = nowString();
  logTime[logHead] = t;
  logUid[logHead] = uid;
  logResult[logHead] = result;
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  Serial.println("[记录] " + t + " 卡号:" + uid + " 结果:" + result);
}

// =====================================================================
// 开锁 / 拒绝(核心行为与阶段4一致)
// =====================================================================

// 放行:绿灯亮 + 舵机开锁3秒 + 自动回锁
// 注意:这里用 delay(3000) 简单实现,教学用够了,但这3秒里网页请求
// (包括 server.handleClient())会被卡住,浏览器会等一下才有响应,属于正常现象。
void grantAccess(String uid) {
  Serial.println("卡号 " + uid + " -> 白名单内,开锁");
  digitalWrite(GREEN_PIN, HIGH);
  lock.write(OPEN_ANGLE);
  delay(UNLOCK_MS);
  lock.write(LOCKED_ANGLE);
  digitalWrite(GREEN_PIN, LOW);
}

// 拒绝:红灯亮 + 蜂鸣器响两声报警
void denyAccess(String uid) {
  Serial.println("卡号 " + uid + " -> 陌生卡,拒绝");
  digitalWrite(RED_PIN, HIGH);
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZ_PIN, HIGH);
    delay(BEEP_MS);
    digitalWrite(BUZZ_PIN, LOW);
    delay(BEEP_MS);
  }
  digitalWrite(RED_PIN, LOW);
}

// 录卡成功后的两声开心短促蜂鸣(和拒绝报警的节奏不一样)
void beepHappy() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZ_PIN, HIGH);
    delay(60);
    digitalWrite(BUZZ_PIN, LOW);
    delay(80);
  }
}

// =====================================================================
// 录卡模式 & 锁定状态的非阻塞状态机(每次 loop 都会调用一次)
// =====================================================================

// 处理录卡模式:双灯闪烁 + 15秒超时自动退出
void handleEnrollBlink() {
  if (!enrollMode || lockoutActive) return;

  if (millis() - enrollStartMs >= ENROLL_MS) {
    enrollMode = false;
    digitalWrite(GREEN_PIN, LOW);
    digitalWrite(RED_PIN, LOW);
    Serial.println("录卡模式超时(15秒),自动退出");
    return;
  }

  if (millis() - lastBlinkMs >= BLINK_MS) {
    lastBlinkMs = millis();
    blinkState = !blinkState;
    digitalWrite(GREEN_PIN, blinkState ? HIGH : LOW);
    digitalWrite(RED_PIN, blinkState ? HIGH : LOW);
  }
}

// 处理锁定状态:红灯常亮 + 每5秒长报警一次 + 30秒后自动解除
void handleLockoutTimer() {
  if (!lockoutActive) return;

  if (millis() - lockoutStartMs >= LOCKOUT_MS) {
    lockoutActive = false;
    denyCount = 0;
    digitalWrite(RED_PIN, LOW);
    Serial.println("锁定结束,恢复正常刷卡");
    return;
  }

  digitalWrite(RED_PIN, HIGH);   // 锁定期间红灯常亮
  if (millis() - lastAlarmMs >= ALARM_INTERVAL_MS) {
    lastAlarmMs = millis();
    digitalWrite(BUZZ_PIN, HIGH);
    delay(500);                  // 长报警声
    digitalWrite(BUZZ_PIN, LOW);
    Serial.println("锁定中,请等待...");
  }
}

// =====================================================================
// 网页看板(仅在WiFi连接成功时启动)
// =====================================================================

void handleRoot() {
  String status;
  if (lockoutActive) status = "锁定中";
  else if (enrollMode) status = "录卡模式";
  else status = "正常";

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>门禁状态</title>";
  html += "<style>body{font-family:sans-serif;margin:16px;background:#f5f5f5;}";
  html += "table{width:100%;border-collapse:collapse;background:#fff;}";
  html += "td,th{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left;}";
  html += "th{background:#eee;}";
  html += ".btn{display:inline-block;padding:10px 16px;margin:6px 8px 12px 0;background:#2ecc71;color:#fff;text-decoration:none;border-radius:6px;}";
  html += ".btn2{background:#e74c3c;}</style>";
  html += "</head><body>";
  html += "<h2>门禁状态看板</h2>";
  html += "<p>当前状态: <b>" + status + "</b></p>";
  html += "<p>白名单卡数: " + String(wlCount) + "</p>";
  html += "<p>当前时间: " + nowString() + "</p>";
  html += "<a class='btn' href='/unlock'>远程开门</a>";
  html += "<a class='btn btn2' href='/reset-deny'>解除锁定/清零计数</a>";
  html += "<h3>最近记录(最多20条)</h3>";
  html += "<table><tr><th>时间</th><th>卡号</th><th>结果</th></tr>";
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - 1 - i + LOG_SIZE) % LOG_SIZE;
    html += "<tr><td>" + logTime[idx] + "</td><td>" + logUid[idx] + "</td><td>" + logResult[idx] + "</td></tr>";
  }
  html += "</table>";
  html += "<p style='color:#888;font-size:12px;'>页面每5秒自动刷新</p>";
  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// /unlock:走一遍和刷白名单卡一样的开锁流程,记录为"远程开门"
void handleUnlock() {
  Serial.println("网页远程开门指令");
  grantAccess("远程");   // 内部有 delay(3000),网页会等开锁完成才跳转回来
  addLog("远程", "远程开门");
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// /reset-deny:清零连续拒绝计数,顺便解除锁定(方便管理员远程恢复)
void handleResetDeny() {
  bool wasLocked = lockoutActive;
  denyCount = 0;
  lockoutActive = false;
  digitalWrite(RED_PIN, LOW);
  Serial.println(wasLocked ? "网页操作:已清零拒绝计数,并解除锁定" : "网页操作:已清零拒绝计数");
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// =====================================================================
// setup / loop
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(GREEN_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(RED_PIN, LOW);
  digitalWrite(BUZZ_PIN, LOW);

  SPI.begin(18, 19, 23, 5);   // SCK, MISO, MOSI, SS
  rfid.PCD_Init();

  lock.attach(SERVO_PIN);
  lock.write(LOCKED_ANGLE);   // 上电先锁上

  prefs.begin("door", false);
  loadWhitelist();

  Serial.println();
  Serial.println("=== 阶段5: 智能升级版 ===");
  Serial.println("白名单已加载,共 " + String(wlCount) + " 张卡");

  connectWiFi();

  if (wifiConnected) {
    server.on("/", handleRoot);
    server.on("/unlock", handleUnlock);
    server.on("/reset-deny", handleResetDeny);
    server.begin();
    Serial.println("网页服务器已启动,浏览器打开上面的IP地址即可查看看板");
  } else {
    Serial.println("WiFi未连接,网页看板不可用,门禁核心功能不受影响");
  }

  Serial.println("刷卡试试... 主卡进入录卡模式,白名单卡开锁,陌生卡报警");
}

void loop() {
  if (wifiConnected) {
    server.handleClient();
  }

  handleLockoutTimer();
  handleEnrollBlink();

  String uid = readUID();
  if (uid == "") return;   // 没读到卡,继续等

  Serial.print("卡号: ");
  Serial.println(uid);

  // 锁定期间,所有卡(包括白名单卡)一律无效
  if (lockoutActive) {
    addLog(uid, "锁定");
    Serial.println("锁定中,此卡被忽略");
    delay(300);
    return;
  }

  // 主卡:切换录卡模式(主卡本身永远不会开门)
  if (uid == MASTER_UID) {
    if (enrollMode) {
      enrollMode = false;
      digitalWrite(GREEN_PIN, LOW);
      digitalWrite(RED_PIN, LOW);
      Serial.println("主卡再次刷卡,取消录卡模式");
    } else {
      enrollMode = true;
      enrollStartMs = millis();
      lastBlinkMs = millis();
      blinkState = true;
      digitalWrite(GREEN_PIN, HIGH);
      digitalWrite(RED_PIN, HIGH);
      Serial.println("检测到主卡,进入录卡模式(15秒内刷一张新卡即可录入)");
    }
    delay(300);
    return;
  }

  // 录卡模式中:下一张(非主卡)卡直接录入白名单并退出录卡模式
  if (enrollMode) {
    bool added = addCard(uid);
    digitalWrite(GREEN_PIN, LOW);
    digitalWrite(RED_PIN, LOW);
    enrollMode = false;
    if (added) {
      addLog(uid, "录卡");
      Serial.println("新卡已录入白名单并保存: " + uid);
    } else {
      Serial.println("此卡已在白名单中,无需重复录入: " + uid);
    }
    beepHappy();
    delay(300);
    return;
  }

  // 正常门禁流程(与阶段4一致)
  if (isAllowed(uid)) {
    grantAccess(uid);
    denyCount = 0;
    addLog(uid, "开门");
  } else {
    denyAccess(uid);
    denyCount++;
    addLog(uid, "拒绝");
    if (denyCount >= DENY_LIMIT) {
      lockoutActive = true;
      lockoutStartMs = millis();
      lastAlarmMs = millis();
      digitalWrite(RED_PIN, HIGH);
      addLog(uid, "锁定");
      Serial.println("连续" + String(DENY_LIMIT) + "次刷卡被拒,进入30秒锁定");
    }
  }

  delay(300);   // 简单防抖:一次刷卡只处理一次
}
