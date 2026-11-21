#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ==========================================
// 1. การตั้งค่าฮาร์ดแวร์และ Pin
// ==========================================
const int dewoSwitchPin = 26; // ขาสวิตช์ Dewo (ต่อกับ GND, ใช้ INPUT_PULLUP)
const int relayPin = 27;      // ขาสั่งงาน Relay ขับมอเตอร์ (Active HIGH)
int extraRunTime = 3200;      // เวลาหน่วงหมุนต่อหลังสวิตช์คลิก (ms)

// ขาเชื่อมต่อจอ LCD (I2C) ของ ESP32
const int sdaPin = 33;        // ขา SDA ของจอ LCD
const int sclPin = 25;        // ขา SCL ของจอ LCD

// ใช้ Address 0x27 ตามค่ามาตรฐาน
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// 2. ข้อมูล Telegram Bot และความปลอดภัย
// ==========================================
#define BOT_TOKEN "9032525697:AAFV4Zs7_dBO92FlexrfwoKFYniM-wIwF5k"
#define BOT_PASSWORD "1234" // รหัสผ่านปลดล็อกใน Telegram

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Preferences prefs;

// ==========================================
// 3. ตัวแปรระบบ Captive Portal (WiFi Setup)
// ==========================================
const char* ap_ssid = "DogFeeder_Setup";
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);

bool inSetupMode = false;
String current_ssid = "";
String current_password = "";
String savedChatId = "";
String authenticatedChatId = "";
int feedCount = 0;
String cachedWifiOptions = ""; // แคชรายชื่อ Wi-Fi ไว้ เพื่อให้หน้าเว็บเปิดได้ทันทีใน 0.05 วินาที ไม่จอขาว

// ==========================================
// 4. ตัวแปรระบบจับเวลาให้อาหาร (Feeding Timer)
// ==========================================
unsigned long timerIntervalMs = 0;   // รอบเวลาจับเวลา (มิลลิวินาที, 0 = ปิด)
unsigned long lastFeedMillis = 0;    // เวลาที่ให้อาหารครั้งล่าสุด
int timerMinutesConfig = 0;          // เก็บจำนวนนาทีที่ตั้งไว้
int roundsPerTimer = 1;              // จำนวนรอบที่จะหมุนเมื่อถึงเวลาจับเวลา

unsigned long lastBotCheck = 0;
const unsigned long botInterval = 1000;
unsigned long lastLcdToggle = 0;
bool lcdToggleState = false;

// ==========================================
// ฟังก์ชันจอ LCD
// ==========================================
void updateLCD(String line1, String line2) {
  lcd.clear();
  delay(5);
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// ==========================================
// ฟังก์ชันกลไกหมุนจ่ายอาหาร 1 รอบ (Dewo Switch)
// ==========================================
void runCalibratedRound() {
  Serial.println(">> เริ่มหมุนมอเตอร์...");
  digitalWrite(relayPin, HIGH);

  // 1. หมุนหนีสวิตช์หากตอนเริ่มถูกกดค้างอยู่
  unsigned long startT = millis();
  while (digitalRead(dewoSwitchPin) == LOW && millis() - startT < 5000) {
    yield();
  }

  // 2. หมุนจนกว่าจะกลับมาชนสวิตช์ Dewo (คลิก = LOW)
  startT = millis();
  while (digitalRead(dewoSwitchPin) == HIGH && millis() - startT < 15000) {
    yield();
  }

  // 3. หน่วงเวลาชดเชยที่ Calibrate ไว้
  if (extraRunTime > 0) {
    delay(extraRunTime);
  }

  // 4. ตัดไฟมอเตอร์
  digitalWrite(relayPin, LOW);
  Serial.println(">> มอเตอร์หยุดตำแหน่งสมบูรณ์!");
}

// ==========================================
// ฟังก์ชันสั่งจ่ายอาหารตามจำนวนรอบที่ระบุ (1-10 รอบ)
// ==========================================
void executeFeeding(int rounds, String chat_id) {
  if (rounds < 1) rounds = 1;
  if (rounds > 10) rounds = 10;

  if (chat_id.length() > 0) {
    bot.sendMessage(chat_id, "⏳ กำลังเริ่มจ่ายอาหาร " + String(rounds) + " รอบ...", "");
  }

  for (int r = 1; r <= rounds; r++) {
    feedCount++;
    prefs.putInt("feedCount", feedCount);

    String l1 = "Feeding: " + String(r) + "/" + String(rounds);
    String l2 = "Total: #" + String(feedCount);
    updateLCD(l1, l2);

    runCalibratedRound();

    if (r < rounds) {
      delay(800); // พัก 0.8 วินาทีระหว่างรอบ
    }
  }

  lastFeedMillis = millis(); // บันทึกเวลาที่จ่ายล่าสุดสำหรับระบบจับเวลา
  updateLCD("Feeder Ready", "Feed Round: #" + String(feedCount));

  if (chat_id.length() > 0) {
    String doneMsg = "🍖 จ่ายอาหารสำเร็จแล้ว " + String(rounds) + " รอบ!\n";
    doneMsg += "• ยอดรวมสะสม: " + String(feedCount) + " รอบ\n";
    if (timerIntervalMs > 0) {
      doneMsg += "⏱️ รอบถัดไปจะให้อัตโนมัติในอีก " + String(timerMinutesConfig) + " นาที";
    }
    bot.sendMessage(chat_id, doneMsg, "");
  }
}

// ==========================================
// หน้าเว็บ HTML สำหรับให้ User เลือก Wi-Fi (Setup)
// ==========================================
// ==========================================
// สแกน Wi-Fi และสร้างหน้าเว็บ HTML (Setup)
// ==========================================
void scanWifiNetworks() {
  Serial.println(">> กำลังสแกนหา Wi-Fi รอบตัว (2.4GHz)...");
  WiFi.disconnect(false); // เคลียร์สถานะ STA ให้พร้อมสแกน
  delay(100);
  WiFi.scanDelete();      // ล้างผลสแกนเดิม
  delay(50);

  int n = WiFi.scanNetworks(false, false); // สแกนคลื่น 2.4GHz
  Serial.printf(">> ผลการสแกน: พบ %d เครือข่าย\n", n);

  cachedWifiOptions = "";
  if (n > 0) {
    for (int i = 0; i < n; ++i) {
      String ssidName = WiFi.SSID(i);
      if (ssidName.length() > 0) {
        cachedWifiOptions += "<option value='" + ssidName + "'>" + ssidName + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
    }
  }
}

String makeSetupPage() {
  if (cachedWifiOptions.length() == 0) {
    scanWifiNetworks();
  }

  String html = "<!DOCTYPE html><html lang='th'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ตั้งค่า Wi-Fi เครื่องให้อาหารสุนัข</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: -apple-system, sans-serif; }";
  html += "body { background: #f0f4f8; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 15px; }";
  html += ".card { background: white; padding: 25px; border-radius: 20px; box-shadow: 0 8px 24px rgba(0,0,0,0.1); width: 100%; max-width: 400px; text-align: center; }";
  html += "h2 { color: #1e3a8a; margin-top: 0; }";
  html += "p { color: #64748b; font-size: 14px; }";
  html += "label { display: block; text-align: left; font-weight: bold; margin-top: 15px; color: #334155; }";
  html += "select, input[type=password], input[type=text] { width: 100%; padding: 12px; margin-top: 6px; border: 1px solid #cbd5e1; border-radius: 10px; font-size: 16px; outline: none; }";
  html += ".btn { background: #2563eb; color: white; border: none; width: 100%; padding: 14px; font-size: 16px; font-weight: bold; border-radius: 10px; margin-top: 25px; cursor: pointer; }";
  html += ".btn:hover { background: #1d4ed8; }";
  html += ".rescan { display: inline-block; margin-top: 15px; color: #2563eb; text-decoration: none; font-size: 14px; }";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h2>🐶 Dog Feeder Setup</h2>";
  html += "<p>กรุณาเลือก Wi-Fi และใส่รหัสผ่าน เพื่อให้เครื่องเชื่อมต่ออินเทอร์เน็ต</p>";
  html += "<form action='/save' method='POST'>";
  html += "<label>เลือกชื่อ Wi-Fi (2.4GHz):</label>";
  html += "<select name='ssid' id='ssid'>";
  if (cachedWifiOptions.length() == 0) html += "<option value=''>-- ไม่พบเครือข่าย หรือพิมพ์ชื่อด้านล่าง --</option>";
  else html += cachedWifiOptions;
  html += "</select>";
  html += "<label>หรือพิมพ์ชื่อ Wi-Fi เอง:</label>";
  html += "<input type='text' name='custom_ssid' placeholder='กรณี Wi-Fi ซ่อน หรือหาชื่อไม่เจอ'>";
  html += "<label>รหัสผ่าน Wi-Fi:</label>";
  html += "<input type='password' name='pass' placeholder='ใส่รหัสผ่าน Wi-Fi' required>";
  html += "<button type='submit' class='btn'>บันทึกและเชื่อมต่อ 🚀</button>";
  html += "</form>";
  html += "<a href='/rescan' class='rescan'>🔄 กดสแกนค้นหา Wi-Fi ใหม่</a>";
  html += "</div></body></html>";
  return html;
}

// ฟังก์ชันตรวจสอบและสั่งให้มือถือ (iOS / Android) เด้งหน้าต่าง Auto-Popup ทันที
bool captivePortalCheck() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "192.168.4.1:80") {
    Serial.println(">> Captive Portal Trigger: " + host + " -> 192.168.4.1");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head><body><a href='http://192.168.4.1/'>Setup WiFi</a></body></html>");
    server.client().stop(); // สำคัญมาก! ต้องปิด socket เพื่อให้มือถือตาม redirect ทันที
    return true;
  }
  return false;
}

// หน้าหลักของเว็บตั้งค่า
void handleRoot() {
  if (captivePortalCheck()) return; // ถ้าไม่ใช่ 192.168.4.1 ให้ redirect เพื่อเปิดหน้าจอ popup ทันที

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", makeSetupPage());
}

void handleRescan() {
  scanWifiNetworks();
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
  server.client().stop();
}

void handleSave() {
  String selectedSSID = server.arg("ssid");
  String customSSID = server.arg("custom_ssid");
  String password = server.arg("pass");
  String finalSSID = (customSSID.length() > 0) ? customSSID : selectedSSID;
  finalSSID.trim();
  password.trim();

  prefs.putString("ssid", finalSSID);
  prefs.putString("pass", password);

  String html = "<!DOCTYPE html><html lang='th'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#f0fdf4;} h2{color:#16a34a;}</style></head><body>";
  html += "<h2>✅ บันทึกสำเร็จ!</h2><p>กำลังเชื่อมต่อไปยัง Wi-Fi: <b>" + finalSSID + "</b></p><p>กรุณารอเครื่องรีสตาร์ทสักครู่...</p></body></html>";
  server.send(200, "text/html", html);

  updateLCD("Connecting to:", finalSSID.substring(0, 16));
  delay(3000);
  ESP.restart();
}

// ฟังก์ชันรองรับการตั้งค่าด่วนผ่าน URL (ถ้ามีการส่ง SSID และ PASS มา)
void handleQuickSet() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("pass");
    newSSID.trim();
    newPass.trim();

    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPass);

    String html = "<!DOCTYPE html><html lang='th'><head><meta charset='UTF-8'>";
    html += "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#f0fdf4;} h2{color:#16a34a;}</style></head><body>";
    html += "<h2>✅ ตั้งค่าสำเร็จ!</h2><p>กำลังเชื่อมต่อ Wi-Fi: <b>" + newSSID + "</b></p></body></html>";
    server.send(200, "text/html", html);

    updateLCD("Connecting to:", newSSID.substring(0, 16));
    delay(2000);
    ESP.restart();
  } else {
    handleRoot();
  }
}

void startSetupMode() {
  inSetupMode = true;
  Serial.println("\n[!] เข้าสู่โหมด Wi-Fi Setup (AP Mode)");
  updateLCD("Connect WiFi:", "DogFeeder_Setup");

  WiFi.disconnect(true); // ปิด connection เก่าทั้งหมด ปลดล็อกชิปวิทยุให้สแกนได้
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_ssid);
  delay(600); // พักรอให้ SoftAP พร้อมทำงาน

  // สแกน Wi-Fi หาเครือข่ายรอบตัว 1 ครั้ง
  scanWifiNetworks();

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", handleRescan);
  server.on("/quickset", handleQuickSet);
  server.on("/favicon.ico", []() { server.send(204); });

  // Captive Portal Endpoints ของทั้ง iOS, Android และ Windows
  server.on("/generate_204", handleRoot);
  server.on("/gen_204", handleRoot);
  server.on("/hotspot-detect.html", handleRoot);
  server.on("/canonical.html", handleRoot);
  server.on("/ncsi.txt", handleRoot);
  server.on("/connecttest.txt", handleRoot);

  server.onNotFound([]() {
    if (captivePortalCheck()) return;
    handleRoot();
  });

  server.begin();
  Serial.println("Wi-Fi AP: " + String(ap_ssid) + " (192.168.4.1)");
}

// ==========================================
// ส่งคีย์บอร์ดเมนูหลัก
// ==========================================
void sendMainMenu(String chat_id, String msg) {
  String keyboard = "[[\"🍖 สั่งจ่ายอาหาร\", \"⏱️ ตั้งเวลาให้อาหาร\"], [\"📊 ดูสถานะ\", \"🔄 รีเซ็ตรอบอาหาร\"], [\"📶 ตั้งค่า Wi-Fi ใหม่\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "", keyboard, true);
}

// ส่งคีย์บอร์ดเลือกรอบอาหาร
void sendRoundSelectionMenu(String chat_id) {
  String msg = "🍖 กรุณาเลือกรอบในการหมุนจ่ายอาหาร:\n(หรือพิมพ์ /feed <จำนวนรอบ> เช่น /feed 5)";
  String keyboard = "[[\"▶️ 1 รอบ\", \"▶️ 2 รอบ\"], [\"▶️ 3 รอบ\", \"▶️ 4 รอบ\"], [\"🔙 กลับเมนูหลัก\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "", keyboard, true);
}

// ส่งคีย์บอร์ดตั้งเวลาจับเวลา
void sendTimerMenu(String chat_id) {
  String msg = "⏱️ ระบบจับเวลาให้อาหารอัตโนมัติ:\n";
  if (timerIntervalMs > 0) {
    msg += "• สถานะปัจจุบัน: ให้อาหารทุก " + String(timerMinutesConfig) + " นาที\n";
  } else {
    msg += "• สถานะปัจจุบัน: ❌ ปิดการจับเวลา (จ่ายอาหารตามสั่งเท่านั้น)\n";
  }
  msg += "\nกรุณาเลือกระยะเวลาที่ต้องการ:";
  String keyboard = "[[\"⏱️ ทุก 4 ชม.\", \"⏱️ ทุก 6 ชม.\"], [\"⏱️ ทุก 8 ชม.\", \"⏱️ ทุก 12 ชม.\"], [\"🧪 ทดสอบ 1 นาที\", \"❌ ปิดระบบจับเวลา\"], [\"🔙 กลับเมนูหลัก\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "", keyboard, true);
}

// ==========================================
// จัดการคำสั่งจาก Telegram
// ==========================================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.println("ข้อความ Telegram: [" + text + "] จาก: " + from_name);

    if (savedChatId != chat_id) {
      savedChatId = chat_id;
      prefs.putString("chatId", savedChatId);
    }

    bool isAuthed = (chat_id == authenticatedChatId);

    // เมนูเริ่มต้น
    if (text == "/start" || text == "/help") {
      if (!isAuthed) {
        String welcome = "🐶 ยินดีต้อนรับสู่เครื่องให้อาหารฟาร์มสุนัข\n\n";
        welcome += "🔒 กรุณาใส่รหัสผ่านเพื่อสั่งงาน:\n";
        welcome += "👉 พิมพ์เลข '" + String(BOT_PASSWORD) + "' ส่งมาได้เลยครับ";
        bot.sendMessage(chat_id, welcome, "");
      } else {
        sendMainMenu(chat_id, "🐶 สวัสดีครับคุณ " + from_name + " เลือกเมนูด้านล่างได้เลยครับ:");
      }
      continue;
    }

    // ล็อกอินรหัสผ่าน
    String enteredPass = "";
    if (text == BOT_PASSWORD) {
      enteredPass = text;
    } else if (text.startsWith("/login ")) {
      enteredPass = text.substring(7);
      enteredPass.trim();
    }

    if (enteredPass.length() > 0) {
      if (enteredPass == BOT_PASSWORD) {
        authenticatedChatId = chat_id;
        sendMainMenu(chat_id, "🎉 รหัสผ่านถูกต้อง! เข้าสู่ระบบสำเร็จ ยินดีต้อนรับคุณ " + from_name);
      } else {
        bot.sendMessage(chat_id, "❌ รหัสผ่านไม่ถูกต้อง! กรุณาลองใหม่อีกครั้ง", "");
      }
      continue;
    }

    if (!isAuthed) {
      bot.sendMessage(chat_id, "⛔ กรุณาพิมพ์รหัสผ่าน (" + String(BOT_PASSWORD) + ") ก่อนสั่งงานครับ", "");
      continue;
    }

    // --- คำสั่งที่ปลดล็อกแล้ว ---

    if (text == "🔙 กลับเมนูหลัก") {
      sendMainMenu(chat_id, "🐶 หน้าเมนูหลัก:");
    }
    // เมนูเลือกรอบอาหาร
    else if (text == "🍖 สั่งจ่ายอาหาร" || text == "/feed") {
      sendRoundSelectionMenu(chat_id);
    }
    // การเลือกจำนวนรอบ
    else if (text == "▶️ 1 รอบ" || text == "1 รอบ") {
      executeFeeding(1, chat_id);
    }
    else if (text == "▶️ 2 รอบ" || text == "2 รอบ") {
      executeFeeding(2, chat_id);
    }
    else if (text == "▶️ 3 รอบ" || text == "3 รอบ") {
      executeFeeding(3, chat_id);
    }
    else if (text == "▶️ 4 รอบ" || text == "4 รอบ") {
      executeFeeding(4, chat_id);
    }
    else if (text.startsWith("/feed ")) {
      int r = text.substring(6).toInt();
      if (r > 0) executeFeeding(r, chat_id);
      else bot.sendMessage(chat_id, "⚠️ กรุณาระบุจำนวนรอบที่ถูกต้อง เช่น /feed 2", "");
    }
    // เมนูระบบจับเวลา
    else if (text == "⏱️ ตั้งเวลาให้อาหาร" || text == "/timer") {
      sendTimerMenu(chat_id);
    }
    else if (text == "⏱️ ทุก 4 ชม.") {
      timerMinutesConfig = 240;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 4 ชั่วโมง");
    }
    else if (text == "⏱️ ทุก 6 ชม.") {
      timerMinutesConfig = 360;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 6 ชั่วโมง");
    }
    else if (text == "⏱️ ทุก 8 ชม.") {
      timerMinutesConfig = 480;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 8 ชั่วโมง");
    }
    else if (text == "⏱️ ทุก 12 ชม.") {
      timerMinutesConfig = 720;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 12 ชั่วโมง");
    }
    else if (text == "🧪 ทดสอบ 1 นาที") {
      timerMinutesConfig = 1;
      timerIntervalMs = 60000UL; // 1 นาที
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "🧪 โหมดทดสอบ: เครื่องจะให้อาหารอัตโนมัติทุก 1 นาที");
    }
    else if (text == "❌ ปิดระบบจับเวลา") {
      timerMinutesConfig = 0;
      timerIntervalMs = 0;
      prefs.putInt("timerMin", 0);
      sendMainMenu(chat_id, "🔒 ปิดระบบจับเวลาเรียบร้อยแล้ว (จะจ่ายเมื่อสั่งผ่านแชทเท่านั้น)");
    }
    else if (text.startsWith("/timer_min ")) {
      int m = text.substring(11).toInt();
      if (m > 0) {
        timerMinutesConfig = m;
        timerIntervalMs = (unsigned long)m * 60000UL;
        prefs.putInt("timerMin", timerMinutesConfig);
        lastFeedMillis = millis();
        sendMainMenu(chat_id, "✅ ตั้งเวลาให้อาหารทุก " + String(m) + " นาที");
      }
    }
    // ดูสถานะ
    else if (text == "/status" || text == "📊 ดูสถานะ") {
      String statusMsg = "📊 รายงานสถานะเครื่องให้อาหาร\n";
      statusMsg += "---------------------------\n";
      statusMsg += "• รอบอาหารสะสม: " + String(feedCount) + " รอบ\n";
      if (timerIntervalMs > 0) {
        unsigned long elapsed = millis() - lastFeedMillis;
        unsigned long remainingSec = (timerIntervalMs > elapsed) ? (timerIntervalMs - elapsed) / 1000 : 0;
        int remM = remainingSec / 60;
        int remS = remainingSec % 60;
        statusMsg += "• ระบบจับเวลา: ทุก " + String(timerMinutesConfig) + " นาที\n";
        statusMsg += "• มื้อถัดไปในอีก: " + String(remM) + " นาที " + String(remS) + " วินาที\n";
      } else {
        statusMsg += "• ระบบจับเวลา: ปิดอยู่ (ให้อาหารตามสั่ง)\n";
      }
      statusMsg += "• Wi-Fi ที่เชื่อมต่อ: " + current_ssid + "\n";
      statusMsg += "• สัญญาณ Wi-Fi: " + String(WiFi.RSSI()) + " dBm\n";
      statusMsg += "• สถานะระบบ: พร้อมทำงาน (Online)";
      bot.sendMessage(chat_id, statusMsg, "");
    }
    // รีเซ็ตรอบอาหาร
    else if (text == "/resetcount" || text == "🔄 รีเซ็ตรอบอาหาร") {
      feedCount = 0;
      prefs.putInt("feedCount", 0);
      updateLCD("Feeder Ready", "Feed Round: #0");
      bot.sendMessage(chat_id, "🔄 รีเซ็ตรอบอาหารสะสมเป็น 0 เรียบร้อยแล้ว", "");
    }
    // ตั้งค่า Wi-Fi ใหม่
    else if (text == "/resetwifi" || text == "📶 ตั้งค่า Wi-Fi ใหม่") {
      bot.sendMessage(chat_id, "⚠️ กำลังล้างค่า Wi-Fi และเปิดโหมดตั้งค่าใหม่\nเครื่องจะปล่อย Wi-Fi ชื่อ 'DogFeeder_Setup' ใน 3 วินาที...", "");
      prefs.remove("ssid");
      prefs.remove("pass");
      delay(3000);
      ESP.restart();
    }
  }
}

// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);

  pinMode(dewoSwitchPin, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  Wire.begin(sdaPin, sclPin);
  Wire.setClock(100000);
  delay(100);

  // เริ่มต้นจอ LCD 1602
  lcd.init();
  Wire.begin(sdaPin, sclPin); // กำหนดขากลับมาที่ SDA=21, SCL=22 ป้องกัน library รีเซ็ตขา
  lcd.backlight();

  updateLCD("Feeder Starting", "Please wait...");
  delay(1500);

  prefs.begin("dog_feeder", false);
  current_ssid = prefs.getString("ssid", "");
  current_password = prefs.getString("pass", "");
  feedCount = prefs.getInt("feedCount", 0);
  savedChatId = prefs.getString("chatId", "");
  timerMinutesConfig = prefs.getInt("timerMin", 0);

  if (timerMinutesConfig > 0) {
    timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
  }

  if (current_ssid.length() == 0) {
    startSetupMode();
    return;
  }

  updateLCD("Connecting WiFi:", current_ssid.substring(0, 16));
  Serial.println("\nกำลังเชื่อมต่อ Wi-Fi: " + current_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(current_ssid.c_str(), current_password.c_str());

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 30) {
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi เชื่อมต่อสำเร็จ!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    secured_client.setInsecure();
    updateLCD("Feeder Ready", "Feed Round: #" + String(feedCount));
    lastFeedMillis = millis();

    if (savedChatId.length() > 0) {
      String onlineMsg = "🐶 เครื่องให้อาหารออนไลน์แล้ว!\n• Wi-Fi: " + current_ssid + "\n• รอบอาหารสะสม: " + String(feedCount) + " รอบ\n";
      if (timerIntervalMs > 0) {
        onlineMsg += "⏱️ ระบบจับเวลา: ทุก " + String(timerMinutesConfig) + " นาที\n";
      }
      bot.sendMessage(savedChatId, onlineMsg, "");
    }
  } else {
    Serial.println("\n❌ ไม่สามารถเชื่อมต่อ Wi-Fi ได้ -> เปิดหน้า Setup");
    updateLCD("WiFi Conn Failed", "Entering Setup..");
    delay(2000);
    WiFi.disconnect(true); // ตัดการเชื่อมต่อที่ค้างอยู่ ปลดล็อกชิปวิทยุให้สแกน Wi-Fi ได้
    startSetupMode();
  }
}

// ==========================================
// Loop
// ==========================================
void loop() {
  if (inSetupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    // 1. ตรวจสอบข้อความ Telegram
    if (millis() - lastBotCheck > botInterval) {
      if (WiFi.status() == WL_CONNECTED) {
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        while (numNewMessages) {
          handleNewMessages(numNewMessages);
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        }
      } else {
        updateLCD("WiFi Disconnect!", "Reconnecting...");
        WiFi.reconnect();
        delay(2000);
      }
      lastBotCheck = millis();
    }

    // 2. ระบบตรวจสอบจับเวลาให้อาหารอัตโนมัติ (Timer Trigger)
    if (timerIntervalMs > 0 && (millis() - lastFeedMillis >= timerIntervalMs)) {
      Serial.println("⏰ ถึงเวลาให้อาหารตามระบบจับเวลาอัตโนมัติ!");
      if (savedChatId.length() > 0) {
        bot.sendMessage(savedChatId, "⏰ [แจ้งเตือนตามเวลา] ถึงรอบให้อาหารอัตโนมัติแล้ว!", "");
      }
      executeFeeding(roundsPerTimer, savedChatId);
    }
  }
}
