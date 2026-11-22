#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <math.h>

// ==========================================
// 1. การตั้งค่าฮาร์ดแวร์และ Pin
// ==========================================
const int dewoSwitchPin = 26; // ขาสวิตช์ Dewo (ต่อกับ GND, ใช้ INPUT_PULLUP)
const int relayPin = 27;      // ขาสั่งงาน Relay ขับมอเตอร์ (Active HIGH)
int extraRunTime = 3200;      // เวลาหน่วงหมุนต่อหลังสวิตช์คลิก (ms)

// ขาเชื่อมต่อจอ LCD (I2C) ของ ESP32
const int sdaPin = 21;        // ขา SDA ของจอ LCD
const int sclPin = 22;        // ขา SCL ของจอ LCD

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
String cachedWifiOptions = ""; // แคชรายชื่อ Wi-Fi

// ==========================================
// 4. ตัวแปรระบบโภชนาการและคำนวณแคลอรี (Dog Nutrition)
// ==========================================
float dogWeight = 10.0;       // น้ำหนักตัวสุนัข (kg)
float dogActivityFactor = 1.6;// ตัวคูณกิจกรรม (1.6 = ปกติ/ทำหมัน)
int mealsPerDay = 2;          // จำนวนมื้อต่อวัน
int dailyKcal = 589;          // แคลอรีที่ต้องการต่อวัน (kcal)
int mealKcal = 295;           // แคลอรีต่อมื้อ (kcal)
int recRoundsPerMeal = 2;     // จำนวนรอบที่คำนวณได้ต่อมื้อ
const float kcalPerGram = 3.6;// พลังงานเฉลี่ยอาหารเม็ด 3.6 kcal/g
const float gramsPerRound = 40.0; // ค่าเฉลี่ยอาหาร 1 รอบการหมุน (~40g)

// ==========================================
// 5. ตัวแปรระบบจับเวลาให้อาหาร (Feeding Timer)
// ==========================================
unsigned long timerIntervalMs = 0;   // รอบเวลาจับเวลา (มิลลิวินาที, 0 = ปิด)
unsigned long lastFeedMillis = 0;    // เวลาที่ให้อาหารครั้งล่าสุด
int timerMinutesConfig = 0;          // เก็บจำนวนนาทีที่ตั้งไว้
int roundsPerTimer = 1;              // จำนวนรอบที่จะหมุนเมื่อถึงเวลาจับเวลา

unsigned long lastBotCheck = 0;
const unsigned long botInterval = 1000;

// ==========================================
// ฟังก์ชันคำนวณแคลอรีตามสูตรสัตวแพทย์
// RER = 70 * (Weight)^0.75
// MER = RER * ActivityFactor
// ==========================================
void calculateNutrition(float weight, float factor, int meals) {
  if (weight <= 0) weight = 5.0;
  if (factor <= 0) factor = 1.6;
  if (meals <= 0) meals = 2;

  dogWeight = weight;
  dogActivityFactor = factor;
  mealsPerDay = meals;

  float rer = 70.0 * pow(dogWeight, 0.75);
  dailyKcal = (int)round(rer * dogActivityFactor);
  mealKcal = (int)round((float)dailyKcal / (float)mealsPerDay);

  float gramsPerMeal = (float)mealKcal / kcalPerGram;
  recRoundsPerMeal = (int)round(gramsPerMeal / gramsPerRound);
  if (recRoundsPerMeal < 1) recRoundsPerMeal = 1;
  if (recRoundsPerMeal > 8) recRoundsPerMeal = 8;
  roundsPerTimer = recRoundsPerMeal; // นำไปใช้ในระบบจับเวลาด้วย
}

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
    bot.sendMessage(chat_id, "⏳ กำลังเริ่มจ่ายอาหาร " + String(rounds) + " รอบ (" + String((int)(rounds * gramsPerRound * kcalPerGram)) + " kcal)...", "");
  }

  for (int r = 1; r <= rounds; r++) {
    feedCount++;
    prefs.putInt("feedCount", feedCount);

    String l1 = "Feeding: " + String(r) + "/" + String(rounds);
    String l2 = "Total: #" + String(feedCount);
    updateLCD(l1, l2);

    runCalibratedRound();

    if (r < rounds) {
      delay(800);
    }
  }

  lastFeedMillis = millis();
  updateLCD("Feeder Ready", "Feed Round: #" + String(feedCount));

  if (chat_id.length() > 0) {
    int totalGivenKcal = (int)(rounds * gramsPerRound * kcalPerGram);
    String doneMsg = "🍖 จ่ายอาหารสำเร็จแล้ว " + String(rounds) + " รอบ!\n";
    doneMsg += "• พลังงานที่จ่าย: ~" + String(totalGivenKcal) + " kcal\n";
    doneMsg += "• ยอดรวมสะสม: " + String(feedCount) + " รอบ\n";
    if (timerIntervalMs > 0) {
      doneMsg += "⏱️ รอบถัดไปจะให้อัตโนมัติในอีก " + String(timerMinutesConfig) + " นาที";
    }
    bot.sendMessage(chat_id, doneMsg, "");
  }
}

// ==========================================
// สแกน Wi-Fi และสร้างหน้าเว็บ HTML (Setup + คำนวณแคลอรี)
// ==========================================
void scanWifiNetworks() {
  Serial.println(">> กำลังสแกนหา Wi-Fi รอบตัว (2.4GHz)...");
  WiFi.disconnect(false);
  delay(100);
  WiFi.scanDelete();
  delay(50);

  int n = WiFi.scanNetworks(false, false);
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
  html += "<title>ตั้งค่า Wi-Fi & คำนวณแคลอรีสุนัข</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: -apple-system, sans-serif; }";
  html += "body { background: #f0f4f8; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 15px; }";
  html += ".card { background: white; padding: 25px; border-radius: 20px; box-shadow: 0 8px 24px rgba(0,0,0,0.1); width: 100%; max-width: 440px; }";
  html += "h2 { color: #1e3a8a; margin-top: 0; text-align: center; }";
  html += ".section-title { font-size: 15px; font-weight: bold; color: #1e40af; border-left: 4px solid #3b82f6; padding-left: 8px; margin: 20px 0 10px; }";
  html += "label { display: block; text-align: left; font-weight: 600; margin-top: 10px; color: #334155; font-size: 13px; }";
  html += "select, input[type=password], input[type=text], input[type=number] { width: 100%; padding: 10px; margin-top: 4px; border: 1px solid #cbd5e1; border-radius: 8px; font-size: 15px; outline: none; }";
  html += ".calc-box { background: #eff6ff; border: 1px solid #bfdbfe; border-radius: 12px; padding: 12px; margin-top: 15px; font-size: 13px; color: #1e3a8a; }";
  html += ".calc-row { display: flex; justify-content: space-between; margin-bottom: 5px; }";
  html += ".calc-val { font-weight: bold; color: #2563eb; }";
  html += ".btn { background: #2563eb; color: white; border: none; width: 100%; padding: 14px; font-size: 16px; font-weight: bold; border-radius: 10px; margin-top: 20px; cursor: pointer; }";
  html += ".btn:hover { background: #1d4ed8; }";
  html += "</style>";
  html += "<script>";
  html += "function calcLive() {";
  html += "  let w = parseFloat(document.getElementById('weight').value) || 10;";
  html += "  let f = parseFloat(document.getElementById('activity').value) || 1.6;";
  html += "  let m = parseInt(document.getElementById('meals').value) || 2;";
  html += "  let rer = 70.0 * Math.pow(w, 0.75);";
  html += "  let mer = Math.round(rer * f);";
  html += "  let perMeal = Math.round(mer / m);";
  html += "  let grams = Math.round(perMeal / 3.6);";
  html += "  let rounds = Math.max(1, Math.min(8, Math.round(grams / 40.0)));";
  html += "  document.getElementById('d_mer').innerText = mer + ' kcal';";
  html += "  document.getElementById('d_meal').innerText = perMeal + ' kcal';";
  html += "  document.getElementById('d_grams').innerText = grams + ' g';";
  html += "  document.getElementById('d_rounds').innerText = rounds + ' รอบ/มื้อ';";
  html += "}";
  html += "</script>";
  html += "</head><body>";
  html += "<div class='card'>";
  html += "<h2>🐶 Dog Feeder Setup</h2>";
  html += "<form action='/save' method='POST'>";

  // ส่วนที่ 1: Wi-Fi
  html += "<div class='section-title'>📶 1. เชื่อมต่อ Wi-Fi บ้าน</div>";
  html += "<label>เลือกชื่อ Wi-Fi (2.4GHz):</label>";
  html += "<select name='ssid' id='ssid'>";
  if (cachedWifiOptions.length() == 0) html += "<option value=''>-- ไม่พบเครือข่าย หรือพิมพ์ชื่อด้านล่าง --</option>";
  else html += cachedWifiOptions;
  html += "</select>";
  html += "<label>หรือพิมพ์ชื่อ Wi-Fi เอง:</label>";
  html += "<input type='text' name='custom_ssid' placeholder='กรณีชื่อ Wi-Fi ซ่อน'>";
  html += "<label>รหัสผ่าน Wi-Fi:</label>";
  html += "<input type='password' name='pass' placeholder='ใส่รหัสผ่าน Wi-Fi' required>";

  // ส่วนที่ 2: คำนวณแคลอรีสุนัข
  html += "<div class='section-title'>🥗 2. คำนวณแคลอรี & โภชนาการสุนัข</div>";
  html += "<label>น้ำหนักตัวสุนัข (กิโลกรัม):</label>";
  html += "<input type='number' step='0.1' name='weight' id='weight' value='" + String(dogWeight, 1) + "' oninput='calcLive()' required>";
  html += "<label>ระดับกิจกรรม / ภาวะสุนัข:</label>";
  html += "<select name='activity' id='activity' onchange='calcLive()'>";
  html += "<option value='1.6'" + String(dogActivityFactor == 1.6f ? " selected" : "") + ">สุนัขโตเต็มวัย (ทำหมันแล้ว / ปกติ)</option>";
  html += "<option value='1.8'" + String(dogActivityFactor == 1.8f ? " selected" : "") + ">สุนัขโตเต็มวัย (ยังไม่ทำหมัน)</option>";
  html += "<option value='1.2'" + String(dogActivityFactor == 1.2f ? " selected" : "") + ">สุนัขต้องการลดน้ำหนัก / อ้วน</option>";
  html += "<option value='2.0'" + String(dogActivityFactor == 2.0f ? " selected" : "") + ">สุนัขกิจกรรมสูง / ฟาร์ม</option>";
  html += "<option value='2.5'" + String(dogActivityFactor == 2.5f ? " selected" : "") + ">ลูกสุนัขกำลังโต</option>";
  html += "</select>";
  html += "<label>จำนวนมื้อต่อวัน:</label>";
  html += "<select name='meals' id='meals' onchange='calcLive()'>";
  html += "<option value='1'" + String(mealsPerDay == 1 ? " selected" : "") + ">1 มื้อ/วัน</option>";
  html += "<option value='2'" + String(mealsPerDay == 2 ? " selected" : "") + ">2 มื้อ/วัน (แนะนำ)</option>";
  html += "<option value='3'" + String(mealsPerDay == 3 ? " selected" : "") + ">3 มื้อ/วัน</option>";
  html += "</select>";

  // กล่องคำนวณสด
  html += "<div class='calc-box'>";
  html += "<div class='calc-row'><span>พลังงานที่ต้องการต่อวัน (MER):</span><span class='calc-val' id='d_mer'>" + String(dailyKcal) + " kcal</span></div>";
  html += "<div class='calc-row'><span>พลังงานต่อมื้อ:</span><span class='calc-val' id='d_meal'>" + String(mealKcal) + " kcal</span></div>";
  html += "<div class='calc-row'><span>ปริมาณอาหารแนะนำต่อมื้อ:</span><span class='calc-val' id='d_grams'>" + String((int)round((float)mealKcal / kcalPerGram)) + " g</span></div>";
  html += "<div class='calc-row'><span>คำนวณเป็นรอบเครื่องหมุน:</span><span class='calc-val' id='d_rounds'>" + String(recRoundsPerMeal) + " รอบ/มื้อ</span></div>";
  html += "</div>";

  html += "<button type='submit' class='btn'>🚀 บันทึกและเริ่มทำงาน</button>";
  html += "</form>";
  html += "</div></body></html>";
  return html;
}

bool captivePortalCheck() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "192.168.4.1:80") {
    Serial.println(">> Captive Portal Trigger: " + host + " -> 192.168.4.1");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head><body><a href='http://192.168.4.1/'>Setup WiFi</a></body></html>");
    server.client().stop();
    return true;
  }
  return false;
}

void handleRoot() {
  if (captivePortalCheck()) return;
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

  float inWeight = server.arg("weight").toFloat();
  float inActivity = server.arg("activity").toFloat();
  int inMeals = server.arg("meals").toInt();

  // คำนวณแคลอรีตามค่าที่ผู้ใช้กรอก
  calculateNutrition(inWeight, inActivity, inMeals);

  // บันทึกลง Preferences ทั้ง Wi-Fi และโภชนาการ
  prefs.putString("ssid", finalSSID);
  prefs.putString("pass", password);
  prefs.putFloat("dogWeight", dogWeight);
  prefs.putFloat("dogAct", dogActivityFactor);
  prefs.putInt("mealsDay", mealsPerDay);
  prefs.putInt("dailyKcal", dailyKcal);
  prefs.putInt("mealKcal", mealKcal);
  prefs.putInt("recRounds", recRoundsPerMeal);

  String html = "<!DOCTYPE html><html lang='th'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;text-align:center;padding:30px;background:#f0fdf4;} .box{background:white;padding:25px;border-radius:16px;max-width:400px;margin:auto;} h2{color:#16a34a;}</style></head><body>";
  html += "<div class='box'>";
  html += "<h2>✅ บันทึกข้อมูลสำเร็จ!</h2>";
  html += "<p>• เชื่อมต่อ Wi-Fi: <b>" + finalSSID + "</b></p>";
  html += "<p>• สุนัขหนัก: <b>" + String(dogWeight, 1) + " กก.</b></p>";
  html += "<p>• แคลอรีต่อวัน: <b>" + String(dailyKcal) + " kcal</b></p>";
  html += "<p>• แคลอรีต่อมื้อ: <b>" + String(mealKcal) + " kcal (" + String(recRoundsPerMeal) + " รอบ)</b></p>";
  html += "<p style='color:#64748b;'>เครื่องกำลังรีสตาร์ทเพื่อเริ่มทำงาน...</p>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);

  updateLCD("Saved! Rebooting", "Target:" + String(mealKcal) + "kcal");
  delay(3000);
  ESP.restart();
}

void startSetupMode() {
  inSetupMode = true;
  Serial.println("\n[!] เข้าสู่โหมด Wi-Fi Setup & Nutrition (AP Mode)");
  updateLCD("Connect WiFi:", "DogFeeder_Setup");

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_ssid);
  delay(600);

  scanWifiNetworks();

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", handleRescan);
  server.on("/favicon.ico", []() { server.send(204); });

  // Captive Portal Endpoints
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
  String keyboard = "[[\"🍖 สั่งจ่ายอาหาร\", \"🥗 โภชนาการ & แคล\"], [\"⏱️ ตั้งเวลาให้อาหาร\", \"📊 ดูสถานะ\"], [\"🔄 รีเซ็ตรอบอาหาร\", \"📶 ตั้งค่า Wi-Fi ใหม่\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "", keyboard, true);
}

// ส่งคีย์บอร์ดเลือกรอบอาหาร
void sendRoundSelectionMenu(String chat_id) {
  String msg = "🍖 กรุณาเลือกรอบในการหมุนจ่ายอาหาร:\n";
  msg += "💡 คำแนะนำ: ตามน้ำหนักตัว (" + String(dogWeight, 1) + " kg) ควรจ่ายมื้อละ " + String(recRoundsPerMeal) + " รอบ (~" + String(mealKcal) + " kcal)";
  String keyboard = "[[\"🎯 จ่ายตามแคล (" + String(recRoundsPerMeal) + " รอบ)\"], [\"▶️ 1 รอบ\", \"▶️ 2 รอบ\"], [\"▶️ 3 รอบ\", \"▶️ 4 รอบ\"], [\"🔙 กลับเมนูหลัก\"]]";
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
    // จ่ายตามแคลที่คำนวณได้
    else if (text.startsWith("🎯 จ่ายตามแคล")) {
      executeFeeding(recRoundsPerMeal, chat_id);
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
    // เมนูโภชนาการและแคลอรี
    else if (text == "🥗 โภชนาการ & แคล" || text == "/nutrition") {
      String nMsg = "🥗 ข้อมูลโภชนาการและแคลอรีสุนัข\n";
      nMsg += "--------------------------------\n";
      nMsg += "• น้ำหนักตัว: " + String(dogWeight, 1) + " kg\n";
      nMsg += "• พลังงานต่อวัน (MER): " + String(dailyKcal) + " kcal\n";
      nMsg += "• จำนวนมื้อ: " + String(mealsPerDay) + " มื้อ/วัน\n";
      nMsg += "• พลังงานต่อมื้อ: ~" + String(mealKcal) + " kcal\n";
      nMsg += "• ปริมาณอาหารแนะนำ: ~" + String((int)round((float)mealKcal / kcalPerGram)) + " กรัม/มื้อ\n";
      nMsg += "• แนะนำจ่าย: " + String(recRoundsPerMeal) + " รอบ/มื้อ\n";
      nMsg += "--------------------------------\n";
      nMsg += "💡 เปลี่ยนน้ำหนักสุนัขได้โดยพิมพ์: /setweight <กก.>\n(เช่น /setweight 12.5)";
      bot.sendMessage(chat_id, nMsg, "");
    }
    else if (text.startsWith("/setweight ")) {
      float w = text.substring(11).toFloat();
      if (w > 0) {
        calculateNutrition(w, dogActivityFactor, mealsPerDay);
        prefs.putFloat("dogWeight", dogWeight);
        prefs.putInt("dailyKcal", dailyKcal);
        prefs.putInt("mealKcal", mealKcal);
        prefs.putInt("recRounds", recRoundsPerMeal);
        String rep = "✅ อัปเดตน้ำหนักเป็น " + String(dogWeight, 1) + " kg เรียบร้อย!\n";
        rep += "• แคลอรีใหม่: " + String(dailyKcal) + " kcal/วัน (มื้อละ " + String(mealKcal) + " kcal)\n";
        rep += "• แนะนำจ่าย: " + String(recRoundsPerMeal) + " รอบ/มื้อ";
        bot.sendMessage(chat_id, rep, "");
      }
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
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 4 ชั่วโมง (" + String(recRoundsPerMeal) + " รอบ/มื้อ)");
    }
    else if (text == "⏱️ ทุก 6 ชม.") {
      timerMinutesConfig = 360;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 6 ชั่วโมง (" + String(recRoundsPerMeal) + " รอบ/มื้อ)");
    }
    else if (text == "⏱️ ทุก 8 ชม.") {
      timerMinutesConfig = 480;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 8 ชั่วโมง (" + String(recRoundsPerMeal) + " รอบ/มื้อ)");
    }
    else if (text == "⏱️ ทุก 12 ชม.") {
      timerMinutesConfig = 720;
      timerIntervalMs = (unsigned long)timerMinutesConfig * 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "✅ ตั้งเวลาสำเร็จ: ให้อาหารอัตโนมัติทุก 12 ชั่วโมง (" + String(recRoundsPerMeal) + " รอบ/มื้อ)");
    }
    else if (text == "🧪 ทดสอบ 1 นาที") {
      timerMinutesConfig = 1;
      timerIntervalMs = 60000UL;
      prefs.putInt("timerMin", timerMinutesConfig);
      lastFeedMillis = millis();
      sendMainMenu(chat_id, "🧪 โหมดทดสอบ: เครื่องจะให้อาหารอัตโนมัติทุก 1 นาที (" + String(recRoundsPerMeal) + " รอบ)");
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
      statusMsg += "• สุนัข: " + String(dogWeight, 1) + " kg (" + String(mealKcal) + " kcal/มื้อ)\n";
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
  Wire.begin(sdaPin, sclPin);
  lcd.backlight();

  updateLCD("Feeder Starting", "Please wait...");
  delay(1500);

  // โหลดค่าจาก Flash Memory
  prefs.begin("dog_feeder", false);
  current_ssid = prefs.getString("ssid", "");
  current_password = prefs.getString("pass", "");
  feedCount = prefs.getInt("feedCount", 0);
  savedChatId = prefs.getString("chatId", "");
  timerMinutesConfig = prefs.getInt("timerMin", 0);

  // โหลดข้อมูลโภชนาการสุนัข
  float savedW = prefs.getFloat("dogWeight", 10.0);
  float savedAct = prefs.getFloat("dogAct", 1.6);
  int savedMeals = prefs.getInt("mealsDay", 2);
  calculateNutrition(savedW, savedAct, savedMeals);

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
    updateLCD("Feeder Ready", "Target:" + String(mealKcal) + "kcal");
    lastFeedMillis = millis();

    if (savedChatId.length() > 0) {
      String onlineMsg = "🐶 เครื่องให้อาหารออนไลน์แล้ว!\n• Wi-Fi: " + current_ssid + "\n";
      onlineMsg += "• สุนัข: " + String(dogWeight, 1) + " kg (" + String(mealKcal) + " kcal/มื้อ)\n";
      onlineMsg += "• รอบอาหารสะสม: " + String(feedCount) + " รอบ\n";
      if (timerIntervalMs > 0) {
        onlineMsg += "⏱️ ระบบจับเวลา: ทุก " + String(timerMinutesConfig) + " นาที (" + String(recRoundsPerMeal) + " รอบ)\n";
      }
      bot.sendMessage(savedChatId, onlineMsg, "");
    }
  } else {
    Serial.println("\n❌ ไม่สามารถเชื่อมต่อ Wi-Fi ได้ -> เปิดหน้า Setup");
    updateLCD("WiFi Conn Failed", "Entering Setup..");
    delay(2000);
    WiFi.disconnect(true);
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
        bot.sendMessage(savedChatId, "⏰ [แจ้งเตือนตามเวลา] ถึงรอบให้อาหารอัตโนมัติ (" + String(recRoundsPerMeal) + " รอบ / ~" + String(mealKcal) + " kcal) แล้ว!", "");
      }
      executeFeeding(recRoundsPerMeal, savedChatId);
    }
  }
}
