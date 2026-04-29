/*
  ============================================================
  ESP32 - Trạm Khí Tượng Thông Minh
  ============================================================
  Phần cứng:
    - DHT11       : GPIO 4
    - LCD 16x2 I2C: SDA=GPIO 21, SCL=GPIO 22 (địa chỉ 0x27)
    - TM1637 (LED 7 thanh 4 chân):
        CLK = GPIO 18
        DIO = GPIO 19
        VCC = 3.3V / 5V
        GND = GND

  Thư viện cần cài (platformio.ini):
    lib_deps =
      adafruit/DHT sensor library @ ^1.4.4
      adafruit/Adafruit Unified Sensor @ ^1.1.9
      marcoschwartz/LiquidCrystal_I2C @ ^1.1.4
      avishorp/TM1637 @ ^1.2.0

  Tính năng:
    - Web server hiển thị nhiệt độ, độ ẩm real-time
    - LCD 16x2: dòng 1 = ngày/tháng/năm, dòng 2 = nhiệt độ & độ ẩm
    - LED 7 thanh TM1637: hiển thị giờ:phút (nhấp nháy dấu hai chấm)
    - NTP đồng bộ thời gian thực
    - Web điều khiển qua điện thoại (responsive)
  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <TM1637Display.h>

// ─── CẤU HÌNH WiFi ───────────────────────────────────────
#define WIFI_SSID     "Trang Tri 3"   // <-- Đổi tên WiFi
#define WIFI_PASSWORD "33333333"      // <-- Đổi mật khẩu

// ─── CHÂN GPIO ───────────────────────────────────────────
#define DHT_PIN       4      // DHT11 data pin
#define DHT_TYPE      DHT11
#define TM_CLK        18     // TM1637 CLK
#define TM_DIO        19     // TM1637 DIO
#define LCD_SDA       21     // I2C SDA (mặc định ESP32)
#define LCD_SCL       22     // I2C SCL (mặc định ESP32)
#define LCD_ADDR      0x27   // Địa chỉ I2C của LCD (thử 0x3F nếu không lên)
#define LCD_COLS      16
#define LCD_ROWS      2

// ─── NTP ─────────────────────────────────────────────────
#define NTP_SERVER    "pool.ntp.org"
#define GMT_OFFSET    25200  // GMT+7 (Việt Nam) = 7 * 3600
#define DST_OFFSET    0

// ─── ĐỐI TƯỢNG ───────────────────────────────────────────
DHT           dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
TM1637Display display(TM_CLK, TM_DIO);
WebServer     server(80);

// ─── BIẾN TOÀN CỤC ──────────────────────────────────────
float temperature = 0.0;
float humidity    = 0.0;
bool  colonState  = true;   // nhấp nháy dấu hai chấm LED
String wifiIP     = "";

unsigned long lastSensorRead = 0;
unsigned long lastLcdUpdate  = 0;
unsigned long lastLedUpdate  = 0;
const unsigned long SENSOR_INTERVAL = 2000;   // đọc cảm biến mỗi 2s
const unsigned long LCD_INTERVAL    = 1000;   // cập nhật LCD mỗi 1s
const unsigned long LED_INTERVAL    = 500;    // nhấp nháy LED mỗi 0.5s

// ─── HTML WEB PAGE (nhúng sẵn vào firmware) ──────────────
// Lưu trong PROGMEM để tiết kiệm RAM
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<title>Trạm Khí Tượng ESP32</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=Rajdhani:wght@300;400;600&display=swap" rel="stylesheet">
<style>
  :root {
    --bg:        #080c14;
    --panel:     #0d1520;
    --border:    #1a2d4a;
    --accent1:   #00d4ff;
    --accent2:   #ff6b35;
    --accent3:   #39ff14;
    --text:      #c8ddf0;
    --dim:       #4a6a8a;
    --glow1:     0 0 20px rgba(0,212,255,0.4);
    --glow2:     0 0 20px rgba(255,107,53,0.4);
    --glow3:     0 0 20px rgba(57,255,20,0.4);
  }
  * { margin:0; padding:0; box-sizing:border-box; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Rajdhani', sans-serif;
    min-height: 100vh;
    overflow-x: hidden;
  }

  /* Grid nền */
  body::before {
    content: '';
    position: fixed; inset: 0;
    background-image:
      linear-gradient(rgba(0,212,255,0.03) 1px, transparent 1px),
      linear-gradient(90deg, rgba(0,212,255,0.03) 1px, transparent 1px);
    background-size: 40px 40px;
    pointer-events: none;
    z-index: 0;
  }

  .container {
    position: relative; z-index: 1;
    max-width: 480px;
    margin: 0 auto;
    padding: 20px 16px 40px;
  }

  /* HEADER */
  .header {
    text-align: center;
    padding: 24px 0 20px;
    border-bottom: 1px solid var(--border);
    margin-bottom: 24px;
  }
  .header-badge {
    display: inline-block;
    font-family: 'Rajdhani', sans-serif;
    font-size: 10px;
    letter-spacing: 4px;
    color: var(--accent1);
    text-transform: uppercase;
    border: 1px solid var(--accent1);
    padding: 4px 12px;
    margin-bottom: 12px;
    opacity: 0.7;
  }
  .header h1 {
    font-family: 'Orbitron', monospace;
    font-size: clamp(18px, 5vw, 26px);
    font-weight: 900;
    color: #fff;
    letter-spacing: 2px;
    text-shadow: 0 0 30px rgba(0,212,255,0.5);
  }
  .header h1 span { color: var(--accent1); }

  /* CLOCK PANEL */
  .clock-panel {
    background: var(--panel);
    border: 1px solid var(--border);
    border-top: 2px solid var(--accent1);
    border-radius: 4px;
    padding: 20px;
    text-align: center;
    margin-bottom: 16px;
    position: relative;
    overflow: hidden;
  }
  .clock-panel::before {
    content: 'REAL-TIME CLOCK';
    position: absolute; top: 8px; left: 12px;
    font-size: 9px; letter-spacing: 3px;
    color: var(--dim); font-family: 'Orbitron', monospace;
  }
  .clock-time {
    font-family: 'Orbitron', monospace;
    font-size: clamp(42px, 14vw, 64px);
    font-weight: 700;
    color: var(--accent1);
    text-shadow: var(--glow1);
    letter-spacing: 4px;
    line-height: 1;
    margin: 12px 0 8px;
  }
  .clock-date {
    font-family: 'Orbitron', monospace;
    font-size: clamp(13px, 3.5vw, 16px);
    color: var(--text);
    letter-spacing: 2px;
    opacity: 0.8;
  }

  /* SENSOR CARDS */
  .sensor-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
    margin-bottom: 16px;
  }
  .sensor-card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 20px 16px;
    text-align: center;
    position: relative;
    overflow: hidden;
    transition: transform 0.2s;
  }
  .sensor-card:active { transform: scale(0.97); }
  .sensor-card.temp { border-top: 2px solid var(--accent2); }
  .sensor-card.humi { border-top: 2px solid var(--accent3); }

  .sensor-card::before {
    content: '';
    position: absolute; bottom: 0; left: 0; right: 0;
    height: 60px;
    opacity: 0.04;
    border-radius: 0 0 4px 4px;
  }
  .sensor-card.temp::before { background: var(--accent2); }
  .sensor-card.humi::before { background: var(--accent3); }

  .sensor-label {
    font-size: 10px; letter-spacing: 3px;
    color: var(--dim); text-transform: uppercase;
    margin-bottom: 10px; font-weight: 600;
  }
  .sensor-icon { font-size: 28px; margin-bottom: 8px; display: block; }
  .sensor-value {
    font-family: 'Orbitron', monospace;
    font-size: clamp(28px, 9vw, 38px);
    font-weight: 700;
    line-height: 1;
  }
  .sensor-unit {
    font-family: 'Rajdhani', sans-serif;
    font-size: 14px; font-weight: 300;
    opacity: 0.6; margin-left: 2px;
  }
  .sensor-card.temp .sensor-value { color: var(--accent2); text-shadow: var(--glow2); }
  .sensor-card.humi .sensor-value { color: var(--accent3); text-shadow: var(--glow3); }

  /* STATUS BAR */
  .status-bar {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 12px 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 16px;
    font-size: 12px;
    letter-spacing: 1px;
  }
  .status-dot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--accent3);
    box-shadow: 0 0 8px var(--accent3);
    display: inline-block;
    margin-right: 8px;
    animation: pulse 2s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50%       { opacity: 0.3; }
  }
  .status-online { color: var(--accent3); }
  .status-ip { color: var(--dim); }

  /* LCD PREVIEW */
  .lcd-preview {
    background: #0a1a0a;
    border: 2px solid #1a3a1a;
    border-radius: 6px;
    padding: 16px 12px;
    margin-bottom: 16px;
    font-family: 'Courier New', monospace;
    box-shadow: inset 0 0 20px rgba(0,255,0,0.05), 0 0 10px rgba(0,255,0,0.1);
    position: relative;
  }
  .lcd-preview::before {
    content: 'LCD 16×2 PREVIEW';
    position: absolute; top: -10px; left: 12px;
    background: var(--bg);
    padding: 0 6px;
    font-size: 9px; letter-spacing: 2px;
    color: #2a6a2a; font-family: 'Rajdhani', sans-serif;
  }
  .lcd-screen {
    background: #0d2b0d;
    border-radius: 3px;
    padding: 10px 14px;
    border: 1px solid #1a4a1a;
  }
  .lcd-row {
    font-size: clamp(13px, 3.5vw, 15px);
    color: #33ff33;
    text-shadow: 0 0 8px rgba(51,255,51,0.6);
    letter-spacing: 1px;
    line-height: 1.8;
    white-space: pre;
  }

  /* UPDATE BUTTON */
  .btn-refresh {
    width: 100%;
    background: transparent;
    border: 1px solid var(--accent1);
    color: var(--accent1);
    font-family: 'Orbitron', monospace;
    font-size: 12px; letter-spacing: 3px;
    padding: 14px;
    border-radius: 4px;
    cursor: pointer;
    text-transform: uppercase;
    transition: all 0.2s;
    position: relative;
    overflow: hidden;
  }
  .btn-refresh::before {
    content: '';
    position: absolute; inset: 0;
    background: var(--accent1);
    opacity: 0;
    transition: opacity 0.2s;
  }
  .btn-refresh:active::before { opacity: 0.1; }
  .btn-refresh:active { transform: scale(0.98); }

  /* Fade-in animation */
  .sensor-value, .clock-time { transition: opacity 0.3s; }
  .updating { opacity: 0.4; }
</style>
</head>
<body>
<div class="container">

  <div class="header">
    <div class="header-badge">ESP32 • DHT11 • TM1637</div>
    <h1>TRẠM <span>KHÍ TƯỢNG</span></h1>
  </div>

  <!-- STATUS -->
  <div class="status-bar">
    <div>
      <span class="status-dot"></span>
      <span class="status-online">ONLINE</span>
    </div>
    <div class="status-ip" id="ipAddr">IP: đang tải...</div>
  </div>

  <!-- CLOCK -->
  <div class="clock-panel">
    <div class="clock-time" id="clockTime">--:--:--</div>
    <div class="clock-date" id="clockDate">-- / -- / ----</div>
  </div>

  <!-- SENSOR -->
  <div class="sensor-grid">
    <div class="sensor-card temp">
      <div class="sensor-label">Nhiệt Độ</div>
      <span class="sensor-icon">🌡️</span>
      <div class="sensor-value" id="tempVal">--<span class="sensor-unit">°C</span></div>
    </div>
    <div class="sensor-card humi">
      <div class="sensor-label">Độ Ẩm</div>
      <span class="sensor-icon">💧</span>
      <div class="sensor-value" id="humiVal">--<span class="sensor-unit">%</span></div>
    </div>
  </div>

  <!-- LCD PREVIEW -->
  <div class="lcd-preview">
    <div class="lcd-screen">
      <div class="lcd-row" id="lcdRow1">-- / -- / ----  </div>
      <div class="lcd-row" id="lcdRow2">T:--C  H:--%   </div>
    </div>
  </div>

  <!-- REFRESH BUTTON -->
  <button class="btn-refresh" onclick="fetchData()">⟳ &nbsp; CẬP NHẬT DỮ LIỆU</button>

</div>

<script>
  // Đồng hồ JS chạy mượt (cập nhật từng giây)
  function updateClock() {
    const now = new Date();
    const h = String(now.getHours()).padStart(2,'0');
    const m = String(now.getMinutes()).padStart(2,'0');
    const s = String(now.getSeconds()).padStart(2,'0');
    const d = String(now.getDate()).padStart(2,'0');
    const mo = String(now.getMonth()+1).padStart(2,'0');
    const y = now.getFullYear();
    document.getElementById('clockTime').textContent = h+':'+m+':'+s;
    document.getElementById('clockDate').textContent = d+' / '+mo+' / '+y;
  }
  setInterval(updateClock, 1000);
  updateClock();

  // Lấy dữ liệu cảm biến từ ESP32
  async function fetchData() {
    try {
      const res  = await fetch('/data');
      const json = await res.json();

      // Nhiệt độ & độ ẩm
      document.getElementById('tempVal').innerHTML =
        json.temp.toFixed(1) + '<span class="sensor-unit">°C</span>';
      document.getElementById('humiVal').innerHTML =
        json.humi.toFixed(1) + '<span class="sensor-unit">%</span>';

      // IP
      document.getElementById('ipAddr').textContent = 'IP: ' + json.ip;

      // LCD preview
      const d  = String(json.day).padStart(2,'0');
      const mo = String(json.month).padStart(2,'0');
      const y  = json.year;
      const t  = json.temp.toFixed(1);
      const h  = json.humi.toFixed(1);
      document.getElementById('lcdRow1').textContent =
        (d+'/'+mo+'/'+y+'      ').substring(0,16);
      document.getElementById('lcdRow2').textContent =
        ('T:'+t+'C H:'+h+'%      ').substring(0,16);
    } catch(e) {
      console.error('Lỗi kết nối:', e);
    }
  }

  // Tự động cập nhật mỗi 3 giây
  fetchData();
  setInterval(fetchData, 3000);
</script>
</body>
</html>
)rawhtml";

// ─── HÀM TIỆN ÍCH ───────────────────────────────────────

// Lấy thời gian hiện tại
struct tm getLocalTime() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) {
    // Nếu chưa sync NTP thì trả về 0
    memset(&timeInfo, 0, sizeof(timeInfo));
  }
  return timeInfo;
}

// ─── WEB ROUTES ──────────────────────────────────────────

// Route: GET / → trả về trang HTML
void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

// Route: GET /data → trả về JSON dữ liệu cảm biến + thời gian
void handleData() {
  struct tm t = getLocalTime();
  char json[256];
  snprintf(json, sizeof(json),
    "{\"temp\":%.1f,\"humi\":%.1f,"
    "\"hour\":%d,\"min\":%d,\"sec\":%d,"
    "\"day\":%d,\"month\":%d,\"year\":%d,"
    "\"ip\":\"%s\"}",
    temperature, humidity,
    t.tm_hour, t.tm_min, t.tm_sec,
    t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
    wifiIP.c_str()
  );
  server.send(200, "application/json", json);
}

// Route: 404
void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// ─── CẬP NHẬT LCD ────────────────────────────────────────
void updateLCD() {
  struct tm t = getLocalTime();
  char row1[17]; // dòng 1: ngày/tháng/năm
  char row2[17]; // dòng 2: nhiệt độ & độ ẩm

  // Dòng 1: DD/MM/YYYY (16 ký tự)
  snprintf(row1, sizeof(row1), "%02d/%02d/%04d      ",
           t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);

  // Dòng 2: T:XX.XC H:XX%
  snprintf(row2, sizeof(row2), "T:%.1fC H:%.1f%%   ",
           temperature, humidity);

  // Giới hạn 16 ký tự
  row1[16] = '\0';
  row2[16] = '\0';

  lcd.setCursor(0, 0); lcd.print(row1);
  lcd.setCursor(0, 1); lcd.print(row2);
}

// ─── CẬP NHẬT LED 7 THANH (TM1637) ──────────────────────
void updateTM1637() {
  struct tm t = getLocalTime();
  // Hiển thị HH:MM, nhấp nháy dấu hai chấm mỗi 0.5s
  int timeVal = t.tm_hour * 100 + t.tm_min;
  uint8_t dots = colonState ? 0b01000000 : 0; // bit dấu hai chấm TM1637
  display.showNumberDecEx(timeVal, dots, true, 4, 0);
  colonState = !colonState;
}

// ─── SETUP ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Trạm khí tượng ESP32");

  // ── DHT11
  dht.begin();
  Serial.println("[OK] DHT11 khởi động");

  // ── LCD
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("  TRAM KHI TUONG");
  lcd.setCursor(0, 1); lcd.print("  Dang khoi dong");
  Serial.println("[OK] LCD khởi động");

  // ── TM1637 — test tuần tự để xác nhận hoạt động
  delay(100);
  display.setBrightness(7, true);   // true = bật output (một số board cần tham số này)

  // Bước 1: bật tất cả segment (8888) trong 1 giây
  uint8_t allOn[] = {0xFF, 0xFF, 0xFF, 0xFF};
  display.setSegments(allOn);
  Serial.println("[TM1637] Test: tất cả segment sáng");
  delay(1000);

  // Bước 2: hiển thị 1234 để kiểm tra từng digit
  display.showNumberDec(1234, false);
  Serial.println("[TM1637] Test: hiển thị 1234");
  delay(1000);

  // Bước 3: hiển thị 00:00
  display.showNumberDecEx(0, 0b01000000, true);
  Serial.println("[OK] TM1637 khởi động");

  // ── WiFi
  Serial.printf("[WiFi] Kết nối tới %s ...\n", WIFI_SSID);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Ket noi WiFi...");
  lcd.setCursor(0, 1); lcd.print(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiIP = WiFi.localIP().toString();
    Serial.printf("\n[WiFi] Kết nối thành công! IP: %s\n", wifiIP.c_str());
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK!");
    lcd.setCursor(0, 1); lcd.print(wifiIP);
    delay(2000);
  } else {
    Serial.println("\n[LỖI] Không kết nối được WiFi!");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Loi WiFi!");
    lcd.setCursor(0, 1); lcd.print("Kiem tra lai...");
    // Tiếp tục chạy offline
  }

  // ── NTP
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  Serial.println("[NTP] Đồng bộ thời gian...");
  struct tm timeInfo;
  int ntpRetry = 0;
  while (!getLocalTime(&timeInfo) && ntpRetry < 10) {
    delay(500); ntpRetry++;
  }
  if (ntpRetry < 10) {
    Serial.printf("[NTP] Đã đồng bộ: %02d/%02d/%04d %02d:%02d:%02d\n",
      timeInfo.tm_mday, timeInfo.tm_mon+1, timeInfo.tm_year+1900,
      timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  } else {
    Serial.println("[LỖI] Không đồng bộ được NTP");
  }

  // ── Web Server
  server.on("/",     HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[HTTP] Web server chạy tại http://%s/\n", wifiIP.c_str());

  // Đọc cảm biến lần đầu
  float h = dht.readHumidity();
  float t2 = dht.readTemperature();
  if (!isnan(h) && !isnan(t2)) {
    humidity    = h;
    temperature = t2;
  }

  // Hiển thị IP trên LCD trước khi bắt đầu
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Web:");
  lcd.setCursor(0, 1); lcd.print(wifiIP);
  delay(3000);
  lcd.clear();

  Serial.println("[READY] Hệ thống sẵn sàng!");
}

// ─── LOOP ────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Xử lý web client
  server.handleClient();

  // Đọc cảm biến DHT11 mỗi 2 giây
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      humidity    = h;
      temperature = t;
      Serial.printf("[SENSOR] Nhiệt độ: %.1f°C  Độ ẩm: %.1f%%\n", t, h);
    } else {
      Serial.println("[LỖI] Đọc DHT11 thất bại, thử lại...");
    }
  }

  // Cập nhật LCD mỗi 1 giây
  if (now - lastLcdUpdate >= LCD_INTERVAL) {
    lastLcdUpdate = now;
    updateLCD();
  }

  // Cập nhật LED 7 thanh mỗi 0.5 giây (nhấp nháy dấu hai chấm)
  if (now - lastLedUpdate >= LED_INTERVAL) {
    lastLedUpdate = now;
    updateTM1637();
  }
}