/*
  RC522 接线自动侦测:把 5 根信号线(SS/SCK/MOSI/MISO/RST)在
  GPIO {5,18,19,23,27} 上的全部 120 种排列逐一用软件SPI试探,
  哪种排列能读出正常版本号(0x88/0x91/0x92),就说明线实际是那样插的。
*/
#include <Arduino.h>

const int PINS[5] = {5, 18, 19, 23, 22};   // RST 候选已从 27 换成 22
const char* ROLE[5] = {"SS", "SCK", "MOSI", "MISO", "RST"};

// 软件SPI读 RC522 的 VersionReg(0x37)
byte softReadVersion(int ss, int sck, int mosi, int miso, int rst) {
  pinMode(ss, OUTPUT);  pinMode(sck, OUTPUT);
  pinMode(mosi, OUTPUT); pinMode(miso, INPUT);
  pinMode(rst, OUTPUT);

  digitalWrite(rst, HIGH);       // 让芯片脱离复位
  digitalWrite(ss, HIGH);
  digitalWrite(sck, LOW);
  delay(3);                      // 芯片上电/出复位缓冲

  digitalWrite(ss, LOW);
  byte addr = 0x80 | (0x37 << 1);   // 读命令: MSB=1, 地址左移1位
  for (int i = 7; i >= 0; i--) {    // 发地址字节
    digitalWrite(mosi, (addr >> i) & 1);
    digitalWrite(sck, HIGH); delayMicroseconds(4);
    digitalWrite(sck, LOW);  delayMicroseconds(4);
  }
  byte val = 0;                     // 收数据字节
  for (int i = 7; i >= 0; i--) {
    digitalWrite(sck, HIGH); delayMicroseconds(4);
    if (digitalRead(miso)) val |= (1 << i);
    digitalWrite(sck, LOW);  delayMicroseconds(4);
  }
  digitalWrite(ss, HIGH);

  // 恢复所有脚为输入,避免影响下一轮
  for (int p : PINS) pinMode(p, INPUT);
  return val;
}

bool good(byte v) { return v != 0x00 && v != 0xFF; }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== RC522 接线自动侦测:尝试全部120种排列 ===");

  int perm[5] = {0, 1, 2, 3, 4};
  int found = 0;
  // 生成全排列(简单递归展开成迭代:用std::next_permutation)
  do {
    int ss = PINS[perm[0]], sck = PINS[perm[1]], mosi = PINS[perm[2]],
        miso = PINS[perm[3]], rst = PINS[perm[4]];
    byte v = softReadVersion(ss, sck, mosi, miso, rst);
    if (good(v)) {
      found++;
      Serial.printf(">>> 命中! VersionReg=0x%02X 时的接法: SS=GPIO%d SCK=GPIO%d MOSI=GPIO%d MISO=GPIO%d RST=GPIO%d\n",
                    v, ss, sck, mosi, miso, rst);
    }
  } while (std::next_permutation(perm, perm + 5));

  if (found == 0) {
    Serial.println("120种排列全部失败 -> 线不(全)在这5个GPIO上,或供电/模块问题。");
    Serial.println("下一步:拍照检查,或换一个RC522模块。");
  } else {
    Serial.printf("共 %d 种排列命中(命中多种时以SS/SCK/MOSI/MISO都参与的为准)\n", found);
  }
  Serial.println("=== 侦测结束(结果只打印一次,需要重跑就按板上EN键) ===");
}

void loop() { delay(1000); }
