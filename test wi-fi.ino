#include <WiFi.h>
#include <WiFiProv.h>

// ฟังก์ชันดักจับสถานะ Wi-Fi
void SysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("\n✅ เชื่อมต่อ Wi-Fi สำเร็จ! IP: ");
      Serial.println(IPAddress(sys_event->event_info.got_ip.ip_info.ip.addr));
      break;
    case ARDUINO_EVENT_PROV_START:
      Serial.println("\n📡 เริ่มเปิดสัญญาณ Bluetooth รอรับการตั้งค่า...");
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("\n🎉 ได้รับรหัส Wi-Fi ผ่าน Bluetooth เรียบร้อยแล้ว!");
      break;
    case ARDUINO_EVENT_PROV_END:
      Serial.println("\n🔒 ปิดระบบ Bluetooth แล้ว เข้าสู่การทำงานปกติ");
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // ดักจับ Event ของการตั้งค่า
  WiFi.onEvent(SysProvEvent);

  // เริ่มระบบ BLE Provisioning
  // - ถ้าเคยต่อ Wi-Fi ได้แล้ว: บอร์ดจะต่อ Wi-Fi อัตโนมัติทันที
  // - ถ้ายังไม่เคยต่อ หรือต่อไม่ติด: บอร์ดจะเปิด Bluetooth ชื่อ "DogFeeder_BLE" ให้อัตโนมัติ
  WiFiProv.beginProvision(
    WIFI_PROV_SCHEME_BLE,      // ใช้โหมด Bluetooth BLE
    WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, 
    WIFI_PROV_SECURITY_0,       // ปลอดภัยระดับพื้นฐาน (ไม่ต้องใส่รหัส PIN เข้าบลูทูธ)
    NULL,                      // Pop token (ถ้ามี)
    "DogFeeder_BLE"            // ชื่อ Bluetooth ที่จะแสดงในมือถือ
  );

  // รอจนกว่าจะต่อ Wi-Fi สำเร็จ
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n>> บอร์ดพร้อมทำงาน ส่งข้อมูลเข้า Telegram ได้เลย!");
}

void loop() {
  // ทำงานตามปกติ เช่น รอคำสั่ง Telegram หรือหมุนมอเตอร์
}
