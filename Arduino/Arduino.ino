#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <user_interface.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266mDNS.h>
#include <time.h>

#include "secrets.h"

// NodeMCU / ESP-12E pin labels
constexpr uint8_t RFID_SS_PIN = D4;
constexpr uint8_t RFID_RST_PIN = D3;
constexpr uint8_t BUZZER_PIN = D0;
constexpr uint8_t OLED_SDA_PIN = D2;
constexpr uint8_t OLED_SCL_PIN = D1;
constexpr uint8_t OLED_ADDRESS = 0x3C; // Change to 0x3D if required by your display.
// Connect battery+ -> 4.7 kOhm -> A0 -> 1 kOhm -> GND.
constexpr float BATTERY_DIVIDER_TOP_RESISTOR_OHMS = 4700.0F;
constexpr float BATTERY_DIVIDER_BOTTOM_RESISTOR_OHMS = 1000.0F;
// Full-scale input on the Wemos/NodeMCU development-board A0 pin.
constexpr float ADC_FULL_SCALE_VOLTS = 3.3F;
constexpr uint8_t BATTERY_SAMPLE_COUNT = 16;
constexpr float BATTERY_VOLTAGE_POINTS[] = {
    3.0F, 3.3F, 3.5F, 3.6F, 3.7F, 3.8F, 3.9F, 4.0F, 4.1F, 4.2F};
constexpr uint8_t BATTERY_PERCENT_POINTS[] = {0, 5, 10, 20, 35, 55, 70, 82, 92, 100};



constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t QUEUE_UPLOAD_INTERVAL_MS = 5000;
constexpr uint32_t DUPLICATE_SCAN_WINDOW_MS = 5000;
constexpr uint32_t MESSAGE_DURATION_MS = 3000;
constexpr uint32_t TIME_ZONE_OFFSET_SECONDS = 19800; // India Standard Time.
constexpr time_t VALID_EPOCH_THRESHOLD = 1704067200;
constexpr uint8_t MAX_TRACKED_TEACHERS = 64;
constexpr uint8_t MAX_STAFF_NAME_LENGTH = 40;
const char* MDNS_HOSTNAME = "attendence";

const char* QUEUE_PATH = "/attendance-queue.csv";
const char* QUEUE_TEMP_PATH = "/attendance-queue.tmp";
const char* QUEUE_BACKUP_PATH = "/attendance-queue.bak";
const char* STATE_PATH = "/attendance-state.csv";
const char* STATE_TEMP_PATH = "/attendance-state.tmp";
const char* STATE_BACKUP_PATH = "/attendance-state.bak";
const char* STAFF_PATH = "/staff.csv";
const char* STAFF_TEMP_PATH = "/staff.tmp";
const char* STAFF_BACKUP_PATH = "/staff.bak";

struct AttendanceState {
  String uid;
  bool present;
  bool hasScanned;
  uint32_t lastScanAt;
};

struct StaffRecord {
  String uid;
  String name;
};

String wifiSsid;
String wifiPassword;
String portalSsid;
String portalPassword;
const char* ADMIN_USERNAME = "admin";
const char* ADMIN_PASSWORD = "password";
String mdnsHostname;
String pendingEnrollmentName;
String enrollmentStatus = "idle";
String enrollmentStatusMessage;
String enrollmentStatusUid;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);
AttendanceState attendanceStates[MAX_TRACKED_TEACHERS];
StaffRecord staffRecords[MAX_TRACKED_TEACHERS];
uint8_t attendanceStateCount = 0;
uint8_t staffRecordCount = 0;
String dashboardTitle;
String dashboardDetail;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastQueueUploadAt = 0;
uint32_t lastDashboardUpdateAt = 0;
uint32_t dashboardMessageUntil = 0;
bool portalActive = false;
bool wifiAttemptStarted = false;
bool networkWasConnected = false;
bool dashboardMessageActive = false;
bool mdnsActive = false;
bool enrollmentPending = false;

const char CONFIG_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RFID Attendance Device</title>
<style>
body{font:16px system-ui,sans-serif;max-width:760px;margin:24px auto;padding:0 16px;color:#172033}
section{border:1px solid #d5dbea;border-radius:12px;padding:18px;margin:16px 0}
input,button{font:inherit;padding:10px;margin:6px 0;max-width:100%;box-sizing:border-box}
input{width:100%;border:1px solid #aeb8ca;border-radius:6px}
button{cursor:pointer;border:0;border-radius:6px;background:#2458c6;color:white}
button.remove{background:#a83232}li{margin:10px 0;word-break:break-word}
.muted{color:#58657a}#enrollment{font-weight:600;min-height:1.5em}
</style></head><body>
<h1>RFID Attendance Device</h1><p id="device" class="muted">Loading device status...</p>
<section><h2>Staff cards</h2>
<form id="enroll"><label>Staff member's name
<input name="name" maxlength="40" required autocomplete="off"></label>
<button type="submit">Start card enrollment</button></form>
<button id="cancel" type="button">Cancel enrollment</button>
<p id="enrollment" aria-live="polite">Enter a name, start enrollment, then scan their card at the device.</p>
<ul id="staff"></ul></section>
<section><h2>Device settings</h2>
<form method="post" action="/save">
<label>Wi-Fi network name (SSID)
<input name="ssid" maxlength="32" required autocomplete="off"></label>
<label>Wi-Fi password (leave blank for an open network)
<input name="password" type="password" maxlength="63" autocomplete="new-password"></label>
<button type="submit">Save Wi-Fi settings</button></form>
<p class="muted">If you keep the same network name, leaving the password blank keeps its current password.</p>
<p class="muted">Staff cards and attendance are stored on the device. Access this page at
the address shown above while connected to the same Wi-Fi. The admin password is shown on
the OLED during Wi-Fi setup and printed to the serial monitor.</p>
</section>
<script>
const staffList=document.getElementById('staff');
const enrollment=document.getElementById('enrollment');
const device=document.getElementById('device');
async function refreshStaff(){
  const response=await fetch('/api/staff');
  if(!response.ok)throw new Error('Could not load staff list.');
  const entries=await response.json();
  staffList.replaceChildren();
  for(const entry of entries){
    const item=document.createElement('li');
    item.append(document.createTextNode(entry.name+' - '+entry.uid+' '));
    const rename=document.createElement('button');
    rename.type='button';rename.textContent='Rename';
    rename.onclick=async()=>{
      const name=prompt('Staff member name',entry.name);
      if(name===null)return;
      const result=await fetch('/api/staff/rename',{method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:new URLSearchParams({uid:entry.uid,name})});
      const data=await result.json();
      if(!result.ok)throw new Error(data.message||'Could not rename staff member.');
      await refreshStaff();
    };
    const remove=document.createElement('button');
    remove.type='button';remove.className='remove';remove.textContent='Remove';
    remove.onclick=async()=>{
      if(!confirm('Remove '+entry.name+' from this device?'))return;
      const result=await fetch('/api/staff/delete',{method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:new URLSearchParams({uid:entry.uid})});
      const data=await result.json();
      if(!result.ok)throw new Error(data.message||'Could not remove staff member.');
      await refreshStaff();
    };
    item.append(rename,remove);staffList.append(item);
  }
}
async function refreshStatus(){
  const response=await fetch('/api/status');
  if(!response.ok)throw new Error('Could not load device status.');
  const data=await response.json();
  device.textContent='Wi-Fi: '+data.wifi+' | Staff: '+data.staff+
    ' | Attendance waiting to sync: '+data.pending+
    ' | Address: http://'+data.host+'.local';
  const ssid=document.querySelector('input[name="ssid"]');
  if(!ssid.value&&data.ssid)ssid.value=data.ssid;
}
document.getElementById('enroll').onsubmit=async(event)=>{
  event.preventDefault();
  const response=await fetch('/api/enroll',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams(new FormData(event.target))});
  const data=await response.json();
  enrollment.textContent=data.message;
  if(!response.ok)throw new Error(data.message);
};
document.getElementById('cancel').onclick=async()=>{
  const response=await fetch('/api/enroll/cancel',{method:'POST'});
  const data=await response.json();enrollment.textContent=data.message;
};
async function poll(){
  try{
    await refreshStatus();await refreshStaff();
    const response=await fetch('/api/enroll/status');
    if(!response.ok)throw new Error('Could not read enrollment status.');
    const data=await response.json();
    if(data.state!=='idle')enrollment.textContent=data.message+(data.uid?' ('+data.uid+')':'');
  }catch(error){device.textContent=error.message;}
}
poll();setInterval(poll,2000);
</script></body></html>
)HTML";

String truncateText(const String& text, uint8_t maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }

  if (maxChars <= 3) {
    return text.substring(0, maxChars);
  }

  return text.substring(0, maxChars - 3) + "...";
}

void drawCenteredText(const String& text, int16_t y, uint8_t textSize) {
  display.setTextSize(textSize);
  display.setTextWrap(false);

  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  display.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);

  const int16_t x = (display.width() - static_cast<int16_t>(w)) / 2;
  display.setCursor(x, y);
  display.print(text);
}

void showMessage(const String& line1, const String& line2 = "");

bool loadWiFiCredentials() {
  File file = LittleFS.open("/wifi.cfg", "r");
  if (!file || file.size() < 2) {
    return false;
  }

  const int ssidLength = file.read();
  const int passwordLength = file.read();
  if (ssidLength < 1 || ssidLength > 32 || passwordLength < 0 ||
      passwordLength > 63 || file.size() != static_cast<size_t>(ssidLength + passwordLength + 2)) {
    file.close();
    return false;
  }

  char ssid[33];
  char password[64];
  if (file.read(reinterpret_cast<uint8_t*>(ssid), ssidLength) != ssidLength ||
      file.read(reinterpret_cast<uint8_t*>(password), passwordLength) != passwordLength) {
    file.close();
    return false;
  }
  file.close();

  ssid[ssidLength] = '\0';
  password[passwordLength] = '\0';
  wifiSsid = ssid;
  wifiPassword = password;
  return true;
}

bool saveWiFiCredentials(const String& ssid, const String& password) {
  if (ssid.length() < 1 || ssid.length() > 32 ||
      (password.length() > 0 && password.length() < 8) || password.length() > 63) {
    return false;
  }

  File file = LittleFS.open("/wifi.cfg", "w");
  if (!file) {
    Serial.println(F("Could not open Wi-Fi configuration for writing."));
    return false;
  }

  file.write(static_cast<uint8_t>(ssid.length()));
  file.write(static_cast<uint8_t>(password.length()));
  const size_t ssidBytesWritten =
      file.write(reinterpret_cast<const uint8_t*>(ssid.c_str()), ssid.length());
  const size_t passwordBytesWritten =
      file.write(reinterpret_cast<const uint8_t*>(password.c_str()), password.length());
  file.close();
  return ssidBytesWritten == ssid.length() &&
         passwordBytesWritten == password.length();
}

bool startConfigPortal() {
  const String chipId = String(ESP.getChipId(), HEX);
  String suffix = chipId;
  suffix.toUpperCase();
  while (suffix.length() < 6) {
    suffix = "0" + suffix;
  }

  portalSsid = "RFID-Setup-" + suffix;
  portalPassword = "RFID" + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(portalSsid.c_str(), portalPassword.c_str())) {
    Serial.println(F("Could not start Wi-Fi setup access point."));
    showMessage("Setup AP failed", "Restart device");
    return false;
  }

  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  portalActive = true;

  Serial.println(F("Wi-Fi setup portal started."));
  Serial.print(F("Connect to: "));
  Serial.println(portalSsid);
  Serial.print(F("Setup password: "));
  Serial.println(portalPassword);
  Serial.println(F("Admin login: admin / password"));
  Serial.println(F("Open http://192.168.4.1 if the setup page does not appear."));
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(F("Setup WiFi"), 2, 2);
  drawCenteredText(portalSsid, 24, 1);
  drawCenteredText("AP: " + portalPassword, 38, 1);
  drawCenteredText(F("Admin: password"), 53, 1);
  display.display();

  return true;
}

void showMessage(const String& line1, const String& line2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (line2.length() == 0) {
    drawCenteredText(truncateText(line1, 16), 18, 2);
    display.display();
    return;
  }

  const String title = truncateText(line1, 12);
  const String detail = truncateText(line2, 18);
  drawCenteredText(title, 8, 2);
  drawCenteredText(detail, 40, 1);
  display.display();
}

void beep(uint16_t durationMs = 100) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void playGreetingSound() {
  beep(70);
  delay(50);
  beep(140);
}

void playGoodbyeSound() {
  beep(150);
  delay(50);
  beep(70);
}

void playQueuedSound() {
  beep(55);
  delay(45);
  beep(55);
  delay(45);
  beep(55);
}

void playErrorSound() {
  beep(350);
}

void setDashboardMessage(const String& title, const String& detail) {
  dashboardTitle = title;
  dashboardDetail = detail;
  dashboardMessageActive = true;
  dashboardMessageUntil = millis() + MESSAGE_DURATION_MS;
  lastDashboardUpdateAt = 0;
}

uint16_t pendingQueueCount();

float readBatteryVoltage() {
  uint32_t adcTotal = 0;
  for (uint8_t sample = 0; sample < BATTERY_SAMPLE_COUNT; sample++) {
    adcTotal += analogRead(A0);
    delayMicroseconds(500);
  }

  const float averageAdc =
      static_cast<float>(adcTotal) / BATTERY_SAMPLE_COUNT;
  const float adcVoltage = averageAdc * ADC_FULL_SCALE_VOLTS / 1023.0F;
  const float dividerRatio =
      (BATTERY_DIVIDER_TOP_RESISTOR_OHMS +
       BATTERY_DIVIDER_BOTTOM_RESISTOR_OHMS) /
      BATTERY_DIVIDER_BOTTOM_RESISTOR_OHMS;
  return adcVoltage * dividerRatio;
}

uint8_t batteryPercentFromVoltage(float voltage) {
  const size_t pointCount =
      sizeof(BATTERY_VOLTAGE_POINTS) / sizeof(BATTERY_VOLTAGE_POINTS[0]);
  if (voltage <= BATTERY_VOLTAGE_POINTS[0]) {
    return BATTERY_PERCENT_POINTS[0];
  }
  if (voltage >= BATTERY_VOLTAGE_POINTS[pointCount - 1]) {
    return BATTERY_PERCENT_POINTS[pointCount - 1];
  }

  for (size_t i = 1; i < pointCount; i++) {
    if (voltage <= BATTERY_VOLTAGE_POINTS[i]) {
      const float segment =
          (voltage - BATTERY_VOLTAGE_POINTS[i - 1]) /
          (BATTERY_VOLTAGE_POINTS[i] - BATTERY_VOLTAGE_POINTS[i - 1]);
      const float percent =
          BATTERY_PERCENT_POINTS[i - 1] +
          segment * (BATTERY_PERCENT_POINTS[i] -
                     BATTERY_PERCENT_POINTS[i - 1]);
      return static_cast<uint8_t>(percent + 0.5F);
    }
  }

  return 100;
}

void renderDashboard() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);

  display.setCursor(0, 0);
  display.print(F("WiFi:"));
  display.print(WiFi.status() == WL_CONNECTED ? F("ON") : F("OFF"));
  display.setCursor(76, 0);
  display.print(F("BAT "));
  display.print(batteryPercentFromVoltage(readBatteryVoltage()));
  display.print('%');

  const time_t now = time(nullptr);
  if (now >= VALID_EPOCH_THRESHOLD) {
    char dateText[20];
    char timeText[12];
    struct tm localTime;
    localtime_r(&now, &localTime);
    strftime(dateText, sizeof(dateText), "%d %b %Y", &localTime);
    strftime(timeText, sizeof(timeText), "%H:%M:%S", &localTime);
    drawCenteredText(dateText, 15, 1);
    drawCenteredText(timeText, 27, 1);
  } else {
    drawCenteredText("Date: --", 15, 1);
    drawCenteredText(WiFi.status() == WL_CONNECTED ? "Time: syncing" : "Time: offline",
                     27, 1);
  }

  if (dashboardMessageActive &&
      static_cast<int32_t>(millis() - dashboardMessageUntil) < 0) {
    drawCenteredText(truncateText(dashboardTitle, 18), 40, 1);
    drawCenteredText(truncateText(dashboardDetail, 21), 53, 1);
  } else {
    dashboardMessageActive = false;
    drawCenteredText(F("Ready to scan"), 42, 1);
    const uint16_t pendingCount = pendingQueueCount();
    if (WiFi.status() != WL_CONNECTED && portalActive) {
      const uint8_t portalScreen = (millis() / 3000) % 3;
      if (portalScreen == 0) {
        drawCenteredText(portalSsid, 56, 1);
      } else if (portalScreen == 1) {
        drawCenteredText("AP: " + portalPassword, 56, 1);
      } else {
        drawCenteredText(F("Admin: password"), 56, 1);
      }
    } else if (pendingCount > 0) {
      display.setCursor(0, 56);
      display.print(F("Pending: "));
      display.print(pendingCount);
    }
  }

  display.display();
}

void beginWiFiConnection() {
  if (wifiSsid.length() == 0) {
    return;
  }

  WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  wifiAttemptStarted = true;
  lastWifiAttemptAt = millis();
  Serial.println(F("Attempting Wi-Fi connection."));
}

void serviceWebServer() {
  if (portalActive) {
    dnsServer.processNextRequest();
  }
  webServer.handleClient();
  if (mdnsActive) {
    MDNS.update();
  }
}

bool checkRfidConnection() {
  Serial.println(F("Checking RC522 SPI connection..."));
  Serial.println(F("Expected: SS=D4, RST=D3, SCK=D5, MISO=D6, MOSI=D7"));

  const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 VersionReg: 0x%02X\n", version);

  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("No RC522 response. Check 3.3V, GND, and SPI/SS/RST wiring."));
    return false;
  }

  if (version == 0x91) {
    Serial.println(F("RC522 v1.0 responded."));
  } else if (version == 0x92) {
    Serial.println(F("RC522 v2.0 responded."));
  } else if (version == 0x88) {
    Serial.println(F("RC522-compatible clone responded."));
  } else {
    Serial.println(F("Reader responded with an unrecognized version; it may be a clone."));
  }
  return true;
}

String readUid() {
  String uid;
  uid.reserve(rfid.uid.size * 2);
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += '0';
    }
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

int8_t findStaffRecord(const String& uid) {
  for (uint8_t i = 0; i < staffRecordCount; i++) {
    if (staffRecords[i].uid == uid) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

String staffNameForUid(const String& uid) {
  const int8_t index = findStaffRecord(uid);
  return index >= 0 ? staffRecords[index].name : uid;
}

bool validStaffName(String& name) {
  name.trim();
  if (name.length() == 0 || name.length() > MAX_STAFF_NAME_LENGTH) {
    return false;
  }

  for (size_t i = 0; i < name.length(); i++) {
    const char character = name[i];
    if (character == ',' || character == '\r' || character == '\n' ||
        static_cast<uint8_t>(character) < 0x20) {
      return false;
    }
  }
  return true;
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    const char character = value[i];
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += character;
    } else if (character == '\n') {
      escaped += F("\\n");
    } else if (character == '\r') {
      escaped += F("\\r");
    } else if (static_cast<uint8_t>(character) >= 0x20) {
      escaped += character;
    }
  }
  return escaped;
}

int8_t findAttendanceState(const String& uid, bool createIfMissing) {
  for (uint8_t i = 0; i < attendanceStateCount; i++) {
    if (attendanceStates[i].uid == uid) {
      return static_cast<int8_t>(i);
    }
  }

  if (!createIfMissing || attendanceStateCount >= MAX_TRACKED_TEACHERS) {
    return -1;
  }

  const uint8_t index = attendanceStateCount++;
  attendanceStates[index].uid = uid;
  attendanceStates[index].present = false;
  attendanceStates[index].hasScanned = false;
  attendanceStates[index].lastScanAt = 0;
  return static_cast<int8_t>(index);
}

bool replaceFile(const char* livePath, const char* tempPath, const char* backupPath) {
  if (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath)) {
    Serial.printf("Could not remove stale backup %s.\n", backupPath);
    return false;
  }

  const bool hadOriginal = LittleFS.exists(livePath);
  if (hadOriginal && !LittleFS.rename(livePath, backupPath)) {
    Serial.printf("Could not back up %s.\n", livePath);
    return false;
  }

  if (!LittleFS.rename(tempPath, livePath)) {
    Serial.printf("Could not replace %s.\n", livePath);
    if (hadOriginal && !LittleFS.rename(backupPath, livePath)) {
      Serial.printf("Could not restore backup for %s.\n", livePath);
    }
    return false;
  }

  if (hadOriginal && LittleFS.exists(backupPath) && !LittleFS.remove(backupPath)) {
    Serial.printf("Could not remove backup %s.\n", backupPath);
  }
  return true;
}

void recoverFile(const char* livePath, const char* backupPath) {
  if (!LittleFS.exists(livePath) && LittleFS.exists(backupPath)) {
    if (!LittleFS.rename(backupPath, livePath)) {
      Serial.printf("Could not recover %s from backup.\n", livePath);
    }
  } else if (LittleFS.exists(livePath) && LittleFS.exists(backupPath)) {
    if (!LittleFS.remove(backupPath)) {
      Serial.printf("Could not remove stale backup %s.\n", backupPath);
    }
  }
}

bool saveStaffDirectory() {
  File file = LittleFS.open(STAFF_TEMP_PATH, "w");
  if (!file) {
    Serial.println(F("Could not open staff directory for writing."));
    return false;
  }

  bool writeSucceeded = true;
  for (uint8_t i = 0; i < staffRecordCount; i++) {
    const String line = staffRecords[i].uid + "," + staffRecords[i].name + "\n";
    if (file.print(line) != line.length()) {
      writeSucceeded = false;
      break;
    }
  }
  file.flush();
  file.close();
  if (!writeSucceeded) {
    Serial.println(F("Could not write complete staff directory."));
    LittleFS.remove(STAFF_TEMP_PATH);
    return false;
  }

  return replaceFile(STAFF_PATH, STAFF_TEMP_PATH, STAFF_BACKUP_PATH);
}

void loadStaffDirectory() {
  File file = LittleFS.open(STAFF_PATH, "r");
  if (!file) {
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    const int separator = line.indexOf(',');
    if (separator <= 0 || staffRecordCount >= MAX_TRACKED_TEACHERS) {
      Serial.println(F("Skipping invalid or excess saved staff record."));
      continue;
    }

    String name = line.substring(separator + 1);
    const String uid = line.substring(0, separator);
    if (!validStaffName(name) || findStaffRecord(uid) >= 0) {
      Serial.println(F("Skipping invalid or duplicate saved staff record."));
      continue;
    }

    staffRecords[staffRecordCount].uid = uid;
    staffRecords[staffRecordCount].name = name;
    staffRecordCount++;
  }
  file.close();
}

bool saveAttendanceStates() {
  File file = LittleFS.open(STATE_TEMP_PATH, "w");
  if (!file) {
    Serial.println(F("Could not open attendance state for writing."));
    return false;
  }

  bool writeSucceeded = true;
  for (uint8_t i = 0; i < attendanceStateCount; i++) {
    const String line = attendanceStates[i].uid +
                        (attendanceStates[i].present ? ",IN\n" : ",OUT\n");
    if (file.print(line) != line.length()) {
      writeSucceeded = false;
      break;
    }
  }
  file.flush();
  file.close();
  if (!writeSucceeded) {
    Serial.println(F("Could not write complete attendance state."));
    LittleFS.remove(STATE_TEMP_PATH);
    return false;
  }

  return replaceFile(STATE_PATH, STATE_TEMP_PATH, STATE_BACKUP_PATH);
}

void loadAttendanceStates() {
  File file = LittleFS.open(STATE_PATH, "r");
  if (!file) {
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    const int separator = line.indexOf(',');
    if (separator <= 0 || attendanceStateCount >= MAX_TRACKED_TEACHERS) {
      Serial.println(F("Skipping invalid or excess saved attendance state."));
      continue;
    }

    const String uid = line.substring(0, separator);
    const String action = line.substring(separator + 1);
    if (action != "IN" && action != "OUT") {
      Serial.println(F("Skipping unknown saved attendance state."));
      continue;
    }

    const int8_t index = findAttendanceState(uid, true);
    if (index >= 0) {
      attendanceStates[index].present = action == "IN";
    }
  }
  file.close();
}

bool appendAttendanceEvent(const String& uid, const String& action, time_t eventTime,
                           const String& staffName) {
  File file = LittleFS.open(QUEUE_PATH, "a");
  if (!file) {
    Serial.println(F("Could not open offline attendance queue."));
    return false;
  }

  const String line = uid + "," + action + "," +
                      String(static_cast<long>(eventTime)) + "," + staffName + "\n";
  const size_t written = file.print(line);
  file.flush();
  file.close();
  if (written != line.length()) {
    Serial.println(F("Could not write complete attendance event to queue."));
    return false;
  }
  return true;
}

uint16_t pendingQueueCount() {
  File file = LittleFS.open(QUEUE_PATH, "r");
  if (!file) {
    return 0;
  }

  uint16_t count = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && count < 65535) {
      count++;
    }
  }
  file.close();
  return count;
}

bool parseAttendanceEvent(const String& line, String& uid, String& action,
                          time_t& eventTime, String& staffName) {
  const int firstSeparator = line.indexOf(',');
  const int secondSeparator = line.indexOf(',', firstSeparator + 1);
  if (firstSeparator <= 0 || secondSeparator <= firstSeparator + 1) {
    return false;
  }

  uid = line.substring(0, firstSeparator);
  action = line.substring(firstSeparator + 1, secondSeparator);
  if (action != "IN" && action != "OUT") {
    return false;
  }
  const int thirdSeparator = line.indexOf(',', secondSeparator + 1);
  const String timestamp = thirdSeparator < 0
                               ? line.substring(secondSeparator + 1)
                               : line.substring(secondSeparator + 1, thirdSeparator);
  eventTime = static_cast<time_t>(timestamp.toInt());
  staffName = thirdSeparator < 0 ? staffNameForUid(uid)
                                 : line.substring(thirdSeparator + 1);
  if (staffName.length() == 0) {
    staffName = staffNameForUid(uid);
  }
  return true;
}

void reconcileQueuedAttendanceState() {
  File file = LittleFS.open(QUEUE_PATH, "r");
  if (!file) {
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    String uid;
    String action;
    String staffName;
    time_t eventTime;
    if (!parseAttendanceEvent(line, uid, action, eventTime, staffName)) {
      Serial.println(F("Skipping invalid queued event while restoring teacher state."));
      continue;
    }

    const int8_t index = findAttendanceState(uid, true);
    if (index < 0) {
      Serial.println(F("Queued event exceeds the teacher state limit."));
      continue;
    }
    attendanceStates[index].present = action == "IN";
  }
  file.close();
}

void setEnrollmentStatus(const String& state, const String& message,
                         const String& uid = "") {
  enrollmentStatus = state;
  enrollmentStatusMessage = message;
  enrollmentStatusUid = uid;
}

void processEnrollmentScan(const String& uid) {
  const String name = pendingEnrollmentName;
  enrollmentPending = false;
  pendingEnrollmentName = "";

  if (findStaffRecord(uid) >= 0) {
    setEnrollmentStatus("error", "That card is already enrolled.", uid);
    setDashboardMessage("Card already used", uid);
    playErrorSound();
    return;
  }

  const int8_t stateIndex = findAttendanceState(uid, true);
  if (staffRecordCount >= MAX_TRACKED_TEACHERS || stateIndex < 0) {
    setEnrollmentStatus("error", "The device staff limit has been reached.", uid);
    setDashboardMessage("Staff list full", "Cannot enroll card");
    playErrorSound();
    return;
  }

  staffRecords[staffRecordCount].uid = uid;
  staffRecords[staffRecordCount].name = name;
  staffRecordCount++;
  if (!saveStaffDirectory()) {
    staffRecordCount--;
    staffRecords[staffRecordCount].uid = "";
    staffRecords[staffRecordCount].name = "";
    setEnrollmentStatus("error", "Could not save staff directory.", uid);
    setDashboardMessage("Enrollment failed", "Storage error");
    playErrorSound();
    return;
  }

  attendanceStates[stateIndex].present = false;
  if (!saveAttendanceStates()) {
    Serial.println(F("Staff was enrolled, but initial attendance state was not saved."));
  }
  setEnrollmentStatus("complete", "Card enrolled for " + name + ".", uid);
  setDashboardMessage("Staff enrolled", name);
  playGreetingSound();
}

bool removeStaffRecord(const String& uid) {
  const int8_t foundIndex = findStaffRecord(uid);
  if (foundIndex < 0) {
    return false;
  }

  const uint8_t index = static_cast<uint8_t>(foundIndex);
  const StaffRecord removedRecord = staffRecords[index];
  for (uint8_t i = index; i + 1 < staffRecordCount; i++) {
    staffRecords[i] = staffRecords[i + 1];
  }
  staffRecordCount--;

  if (!saveStaffDirectory()) {
    for (uint8_t i = staffRecordCount; i > index; i--) {
      staffRecords[i] = staffRecords[i - 1];
    }
    staffRecords[index] = removedRecord;
    staffRecordCount++;
    return false;
  }

  const int8_t stateIndex = findAttendanceState(uid, false);
  if (stateIndex >= 0) {
    attendanceStates[stateIndex].present = false;
    if (!saveAttendanceStates()) {
      Serial.println(F("Staff was removed, but attendance state could not be updated."));
    }
  }
  return true;
}

bool renameStaffRecord(const String& uid, String name) {
  const int8_t index = findStaffRecord(uid);
  if (index < 0 || !validStaffName(name)) {
    return false;
  }

  const String previousName = staffRecords[index].name;
  staffRecords[index].name = name;
  if (saveStaffDirectory()) {
    return true;
  }
  staffRecords[index].name = previousName;
  return false;
}

bool uploadAttendance(const String& uid, const String& staffName,
                     const String& action, time_t eventTime) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // TLS is encrypted, but setInsecure() does not verify the Supabase server certificate.
  // Configure a trusted CA with BearSSL for production deployments.
  BearSSL::WiFiClientSecure tlsClient;
  tlsClient.setInsecure();

  HTTPClient http;
  const String endpoint = String(SUPABASE_URL) + "/rest/v1/attendance";
  if (!http.begin(tlsClient, endpoint)) {
    Serial.println(F("Could not start HTTPS request."));
    return false;
  }

  http.setTimeout(4000);
  http.addHeader(F("Content-Type"), F("application/json"));
  http.addHeader(F("apikey"), SUPABASE_ANON_KEY);
  http.addHeader(F("Authorization"),
                 String(F("Bearer ")) + SUPABASE_ANON_KEY);
  http.addHeader(F("Prefer"), F("return=minimal"));

  String body = String(F("{\"uid\":\"")) + uid +
                F("\",\"staff_name\":\"") + jsonEscape(staffName) +
                F("\",\"device_id\":\"") + DEVICE_ID +
                F("\",\"action\":\"") + action + F("\"");
  if (eventTime >= VALID_EPOCH_THRESHOLD) {
    struct tm eventLocalTime;
    char timestamp[32];
    localtime_r(&eventTime, &eventLocalTime);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &eventLocalTime);
    body += String(F(",\"timestamp\":\"")) + timestamp + F("\"");
  }
  body += "}";
  const int statusCode = http.POST(body);

  if (statusCode > 0) {
    Serial.printf("Supabase HTTP status: %d\n", statusCode);
    if (statusCode >= 300) {
      Serial.println(F("Supabase response body:"));
      Serial.println(http.getString());
    }
  } else {
    Serial.printf("Supabase request failed: %s\n", http.errorToString(statusCode).c_str());
  }
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

bool uploadNextQueuedAttendance() {
  File queueFile = LittleFS.open(QUEUE_PATH, "r");
  if (!queueFile) {
    return true;
  }

  String firstLine;
  while (queueFile.available() && firstLine.length() == 0) {
    firstLine = queueFile.readStringUntil('\n');
    firstLine.trim();
  }
  if (firstLine.length() == 0) {
    queueFile.close();
    return true;
  }

  String uid;
  String action;
  String staffName;
  time_t eventTime;
  if (!parseAttendanceEvent(firstLine, uid, action, eventTime, staffName)) {
    Serial.println(F("Invalid attendance queue entry; retaining it for inspection."));
    queueFile.close();
    return false;
  }

  File tempFile = LittleFS.open(QUEUE_TEMP_PATH, "w");
  if (!tempFile) {
    Serial.println(F("Could not open temporary attendance queue."));
    queueFile.close();
    return false;
  }

  bool writeSucceeded = true;
  while (queueFile.available()) {
    const String remainder = queueFile.readStringUntil('\n');
    if (remainder.length() > 0 && tempFile.print(remainder + "\n") != remainder.length() + 1) {
      writeSucceeded = false;
      break;
    }
  }
  queueFile.close();
  tempFile.flush();
  tempFile.close();

  if (!writeSucceeded) {
    Serial.println(F("Could not prepare updated attendance queue."));
    LittleFS.remove(QUEUE_TEMP_PATH);
    return false;
  }

  if (!uploadAttendance(uid, staffName, action, eventTime)) {
    LittleFS.remove(QUEUE_TEMP_PATH);
    return false;
  }

  return replaceFile(QUEUE_PATH, QUEUE_TEMP_PATH, QUEUE_BACKUP_PATH);
}

void serviceNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!networkWasConnected) {
      networkWasConnected = true;
      Serial.print(F("Wi-Fi connected; IP: "));
      Serial.println(WiFi.localIP());
      if (!mdnsActive) {
        mdnsActive = MDNS.begin(mdnsHostname.c_str());
        if (mdnsActive) {
          MDNS.addService("http", "tcp", 80);
          Serial.print(F("Settings: http://"));
          Serial.print(mdnsHostname);
          Serial.println(F(".local/"));
        } else {
          Serial.println(F("mDNS startup failed; use the device IP address."));
        }
      }
      configTime(TIME_ZONE_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov");
      setDashboardMessage("WiFi connected", "Syncing clock");
    }

    if (millis() - lastQueueUploadAt >= QUEUE_UPLOAD_INTERVAL_MS) {
      lastQueueUploadAt = millis();
      uploadNextQueuedAttendance();
    }
    return;
  }

  if (mdnsActive) {
    MDNS.close();
    mdnsActive = false;
  }
  networkWasConnected = false;
  if (wifiSsid.length() == 0) {
    if (!portalActive &&
        (!wifiAttemptStarted ||
         millis() - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS)) {
      wifiAttemptStarted = true;
      lastWifiAttemptAt = millis();
      startConfigPortal();
    }
    return;
  }

  if (!wifiAttemptStarted ||
      millis() - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS) {
    if (wifiAttemptStarted && !portalActive) {
      startConfigPortal();
    }
    beginWiFiConnection();
  }
}

bool authenticateAdmin() {
  if (webServer.authenticate(ADMIN_USERNAME, ADMIN_PASSWORD)) {
    return true;
  }
  webServer.requestAuthentication(BASIC_AUTH, "RFID Attendance");
  return false;
}

void sendApiError(int statusCode, const String& message) {
  webServer.send(statusCode, "application/json",
                 String(F("{\"message\":\"")) + jsonEscape(message) + F("\"}"));
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, []() {
    if (!authenticateAdmin()) {
      return;
    }
    webServer.send_P(200, "text/html", CONFIG_PAGE);
  });

  webServer.on("/save", HTTP_POST, []() {
    if (!authenticateAdmin()) {
      return;
    }
    if (!webServer.hasArg("ssid") || !webServer.hasArg("password")) {
      sendApiError(400, "SSID and password fields are required.");
      return;
    }

    const String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");
    if (password.length() == 0 && ssid == wifiSsid) {
      password = wifiPassword;
    }
    if (!saveWiFiCredentials(ssid, password)) {
      sendApiError(400, "Invalid Wi-Fi credentials or could not save configuration.");
      return;
    }

    wifiSsid = ssid;
    wifiPassword = password;
    WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
    beginWiFiConnection();
    webServer.send(200, "text/html",
                   "<!doctype html><html><body><h3>Wi-Fi settings saved</h3>"
                   "<p>The device is reconnecting. Card scanning remains available.</p>"
                   "<p><a href=\"/\">Return to settings</a></p></body></html>");
  });

  webServer.on("/api/status", HTTP_GET, []() {
    if (!authenticateAdmin()) {
      return;
    }
    const String wifiState =
        WiFi.status() == WL_CONNECTED ? "connected" : "offline";
    const String response =
        String(F("{\"wifi\":\"")) + wifiState +
        F("\",\"ssid\":\"") + jsonEscape(wifiSsid) +
        F("\",\"staff\":") + String(staffRecordCount) +
        F(",\"pending\":") + String(pendingQueueCount()) +
        F(",\"host\":\"") + jsonEscape(mdnsHostname) + F("\"}");
    webServer.send(200, "application/json", response);
  });

  webServer.on("/api/staff", HTTP_GET, []() {
    if (!authenticateAdmin()) {
      return;
    }
    String response = "[";
    for (uint8_t i = 0; i < staffRecordCount; i++) {
      if (i > 0) {
        response += ',';
      }
      response += String(F("{\"uid\":\"")) + jsonEscape(staffRecords[i].uid) +
                  F("\",\"name\":\"") + jsonEscape(staffRecords[i].name) + F("\"}");
    }
    response += ']';
    webServer.send(200, "application/json", response);
  });

  webServer.on("/api/enroll", HTTP_POST, []() {
    if (!authenticateAdmin()) {
      return;
    }
    if (!webServer.hasArg("name")) {
      sendApiError(400, "A staff name is required.");
      return;
    }
    String name = webServer.arg("name");
    if (!validStaffName(name)) {
      sendApiError(400, "Enter a name of 1 to 40 characters without commas.");
      return;
    }
    if (enrollmentPending) {
      sendApiError(409, "Finish the current card enrollment first.");
      return;
    }
    if (staffRecordCount >= MAX_TRACKED_TEACHERS) {
      sendApiError(409, "The device staff limit has been reached.");
      return;
    }

    pendingEnrollmentName = name;
    enrollmentPending = true;
    setEnrollmentStatus("waiting", "Scan a card for " + name + ".");
    setDashboardMessage("Enroll card now", name);
    webServer.send(202, "application/json",
                   String(F("{\"message\":\"")) +
                       jsonEscape(enrollmentStatusMessage) + F("\"}"));
  });

  webServer.on("/api/enroll/status", HTTP_GET, []() {
    if (!authenticateAdmin()) {
      return;
    }
    const String response =
        String(F("{\"state\":\"")) + jsonEscape(enrollmentStatus) +
        F("\",\"message\":\"") + jsonEscape(enrollmentStatusMessage) +
        F("\",\"uid\":\"") + jsonEscape(enrollmentStatusUid) + F("\"}");
    webServer.send(200, "application/json", response);
  });

  webServer.on("/api/enroll/cancel", HTTP_POST, []() {
    if (!authenticateAdmin()) {
      return;
    }
    enrollmentPending = false;
    pendingEnrollmentName = "";
    setEnrollmentStatus("idle", "Card enrollment canceled.");
    webServer.send(200, "application/json",
                   "{\"message\":\"Card enrollment canceled.\"}");
  });

  webServer.on("/api/staff/rename", HTTP_POST, []() {
    if (!authenticateAdmin()) {
      return;
    }
    if (!webServer.hasArg("uid") || !webServer.hasArg("name")) {
      sendApiError(400, "A card UID and staff name are required.");
      return;
    }
    String name = webServer.arg("name");
    if (!validStaffName(name)) {
      sendApiError(400, "Enter a name of 1 to 40 characters without commas.");
      return;
    }
    if (!renameStaffRecord(webServer.arg("uid"), name)) {
      sendApiError(404, "Staff card was not found or the directory could not be saved.");
      return;
    }
    webServer.send(200, "application/json", "{\"message\":\"Staff name updated.\"}");
  });

  webServer.on("/api/staff/delete", HTTP_POST, []() {
    if (!authenticateAdmin()) {
      return;
    }
    if (!webServer.hasArg("uid")) {
      sendApiError(400, "A card UID is required.");
      return;
    }
    if (!removeStaffRecord(webServer.arg("uid"))) {
      sendApiError(404, "Staff card was not found or the directory could not be saved.");
      return;
    }
    webServer.send(200, "application/json", "{\"message\":\"Staff card removed.\"}");
  });

  webServer.onNotFound([]() {
    if (portalActive) {
      webServer.sendHeader("Location", "http://192.168.4.1/", true);
      webServer.send(302, "text/plain", "");
    } else {
      webServer.send(404, "text/plain", "Not found");
    }
  });
  webServer.begin();
}

void initializeDeviceIdentity() {
  String suffix = String(ESP.getChipId(), HEX);
  suffix.toUpperCase();
  while (suffix.length() < 6) {
    suffix = "0" + suffix;
  }
  portalPassword = "RFID" + suffix;
  mdnsHostname = MDNS_HOSTNAME;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("SSD1306 initialization failed."));
    while (true) {
      delay(1000);
    }
  }
  showMessage("RFID Attendance", "Starting...");

  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount failed; refusing to format and erase attendance data."));
    showMessage("Storage error", "Data not erased");
    while (true) {
      delay(1000);
    }
  }

  initializeDeviceIdentity();

  recoverFile(QUEUE_PATH, QUEUE_BACKUP_PATH);
  recoverFile(STATE_PATH, STATE_BACKUP_PATH);
  recoverFile(STAFF_PATH, STAFF_BACKUP_PATH);
  loadStaffDirectory();
  loadAttendanceStates();
  reconcileQueuedAttendanceState();

  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  if (checkRfidConnection()) {
    Serial.println(F("RC522 ready. Scan a card."));
  } else {
    Serial.println(F("RC522 not found; continuing so Wi-Fi and status remain available."));
    setDashboardMessage("RC522 not found", "Check SPI wiring");
  }

  Serial.println(F("Admin login: admin / password"));
  Serial.print(F("Device settings host: http://"));
  Serial.print(mdnsHostname);
  Serial.println(F(".local/ (available after Wi-Fi connects)"));
  setupWebServer();

  if (loadWiFiCredentials()) {
    beginWiFiConnection();
  } else {
    Serial.println(F("No saved Wi-Fi; starting setup portal."));
    wifiAttemptStarted = true;
    lastWifiAttemptAt = millis();
    startConfigPortal();
  }
  renderDashboard();
}

void loop() {
  serviceWebServer();
  serviceNetwork();

  if (millis() - lastDashboardUpdateAt >= 1000) {
    lastDashboardUpdateAt = millis();
    renderDashboard();
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    const String uid = readUid();
    Serial.print(F("Card UID: "));
    Serial.println(uid);

    if (enrollmentPending) {
      processEnrollmentScan(uid);
    } else {
      const int8_t staffIndex = findStaffRecord(uid);
      if (staffIndex < 0) {
        Serial.println(F("Card is not enrolled; use the device web settings to enroll it."));
        setDashboardMessage("Card not enrolled", "Use staff settings");
        playErrorSound();
      } else {
        const uint32_t scanTime = millis();
        const int8_t stateIndex = findAttendanceState(uid, true);
        if (stateIndex < 0) {
          Serial.println(F("Teacher state limit reached; attendance not recorded."));
          setDashboardMessage("Storage limit", "Teacher list full");
          playErrorSound();
        } else if (attendanceStates[stateIndex].hasScanned &&
                   scanTime - attendanceStates[stateIndex].lastScanAt <
                       DUPLICATE_SCAN_WINDOW_MS) {
          Serial.println(F("Ignoring repeated read inside the 5-second duplicate window."));
        } else {
          const String action =
              attendanceStates[stateIndex].present ? "OUT" : "IN";
          const String staffName = staffRecords[staffIndex].name;
          const time_t currentTime = time(nullptr);
          const time_t eventTime =
              currentTime >= VALID_EPOCH_THRESHOLD ? currentTime : 0;
          if (!appendAttendanceEvent(uid, action, eventTime, staffName)) {
            setDashboardMessage("Attendance error", "Could not save");
            playErrorSound();
          } else {
            attendanceStates[stateIndex].present = action == "IN";
            attendanceStates[stateIndex].hasScanned = true;
            attendanceStates[stateIndex].lastScanAt = scanTime;
            const bool stateSaved = saveAttendanceStates();
            if (!stateSaved) {
              Serial.println(F("Attendance is queued, but teacher state was not persisted."));
            }

            if (action == "IN") {
              setDashboardMessage("Welcome", staffName);
              playGreetingSound();
            } else {
              setDashboardMessage("Goodbye", staffName);
              playGoodbyeSound();
            }

            if (WiFi.status() != WL_CONNECTED) {
              Serial.println(F("Attendance saved locally; waiting for Wi-Fi."));
              playQueuedSound();
            }
          }
        }
      }
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  delay(10);
}