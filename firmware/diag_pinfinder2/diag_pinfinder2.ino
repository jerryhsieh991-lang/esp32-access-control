/*
  RC522 接线全域侦测 v2:
  在 16 个常用 GPIO 上搜索 SS/SCK/MOSI/MISO 四根线的全部排列(16P4=43680种)。
  技巧:先把所有候选脚全部拉高 —— RST 无论插在哪都会被拉高(芯片出复位),
  SS 拉高=未选中,互不干扰;然后逐组合试探读 VersionReg。
*/
#include <Arduino.h>

const int PINS[] = {4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
const int N = sizeof(PINS) / sizeof(PINS[0]);

void allHigh() {
  for (int i = 0; i < N; i++) { pinMode(PINS[i], OUTPUT); digitalWrite(PINS[i], HIGH); }
}

byte probe(int ss, int sck, int mosi, int miso) {
  pinMode(miso, INPUT);
  digitalWrite(sck, LOW);
  delayMicroseconds(20);
  digitalWrite(ss, LOW);
  delayMicroseconds(10);
  byte addr = 0x80 | (0x37 << 1);   // 读 VersionReg
  for (int i = 7; i >= 0; i--) {
    digitalWrite(mosi, (addr >> i) & 1);
    digitalWrite(sck, HIGH); delayMicroseconds(3);
    digitalWrite(sck, LOW);  delayMicroseconds(3);
  }
  byte val = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(sck, HIGH); delayMicroseconds(3);
    if (digitalRead(miso)) val |= (1 << i);
    digitalWrite(sck, LOW);  delayMicroseconds(3);
  }
  digitalWrite(ss, HIGH);
  pinMode(miso, OUTPUT); digitalWrite(miso, HIGH);   // 恢复拉高
  return val;
}

bool good(byte v) { return v != 0x00 && v != 0xFF; }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== RC522 全域接线侦测 v2:%d 个候选脚,%d 种组合 ===\n", N, N*(N-1)*(N-2)*(N-3));
  allHigh();
  delay(50);   // 等 RC522 出复位

  long tried = 0; int found = 0;
  for (int a = 0; a < N; a++)
  for (int b = 0; b < N; b++) { if (b == a) continue;
  for (int c = 0; c < N; c++) { if (c == a || c == b) continue;
  for (int d = 0; d < N; d++) { if (d == a || d == b || d == c) continue;
    byte v = probe(PINS[a], PINS[b], PINS[c], PINS[d]);
    tried++;
    if (good(v)) {
      found++;
      Serial.printf(">>> 命中! Version=0x%02X : SS=GPIO%d SCK=GPIO%d MOSI=GPIO%d MISO=GPIO%d\n",
                    v, PINS[a], PINS[b], PINS[c], PINS[d]);
    }
    if (tried % 10000 == 0) Serial.printf("...已试 %ld 种\n", tried);
  }}}

  if (found == 0) {
    Serial.println("全部组合失败 -> 结论:不是接线顺序问题。");
    Serial.println("剩下三种可能:1) RC522 没供上电(量VCC-GND电压) 2) 杜邦线内部断了(换线) 3) 模块坏了");
  } else {
    Serial.printf("共 %d 个命中,取出现次数最多的那组就是真实接线\n", found);
  }
  Serial.println("=== 侦测结束(重跑按板上EN键)===");
  for (int i = 0; i < N; i++) pinMode(PINS[i], INPUT);
}

void loop() { delay(1000); }
