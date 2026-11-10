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

// NodeMCU / ESP-12E pin labels
constexpr uint8_t RFID_SS_PIN = D4;
constexpr uint8_t RFID_RST_PIN = D3;
constexpr uint8_t BUZZER_PIN = D0;
constexpr uint8_t OLED_SDA_PIN = D2;
constexpr uint8_t OLED_SCL_PIN = D1;
constexpr uint8_t OLED_ADDRESS = 0x3C; // Change to 0x3D if required by your display.

// Set to 1 to enable the optional A0 battery divider measurement.
#define ENABLE_BATTERY_MONITOR 0

// These values assume a NodeMCU board with a 0-3.3 V A0 input and a 100k/100k divider.
// For a bare ESP8266 ADC input, set ADC_FULL_SCALE_VOLTS to 1.0 and use a suitable divider.
constexpr float ADC_FULL_SCALE_VOLTS = 3.30f;
constexpr float BATTERY_R_TOP_OHMS = 100000.0f;
constexpr float BATTERY_R_BOTTOM_OHMS = 100000.0f;

const char* SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";
const char* DEVICE_ID = "attendance-device-01";

String wifiSsid;
String wifiPassword;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

const char CONFIG_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RFID Attendance Wi-Fi Setup</title></head>
<body style="font:16px sans-serif;max-width:420px;margin:40px auto;padding:0 16px">
<h2>Wi-Fi setup</h2><p>Enter the Wi-Fi network for this attendance device.</p>
<form method="post" action="/save">
<label>Wi-Fi name (SSID)<br><input name="ssid" maxlength="32" required style="width:100%;padding:10px"></label><br><br>
<label>Wi-Fi password (leave blank for an open network)<br>
<input name="password" type="password" maxlength="63" style="width:100%;padding:10px"></label><br><br>
<button style="padding:10px 18px">Save and restart</button></form></body></html>
)HTML";

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

void startConfigPortal() {
  const String chipId = String(ESP.getChipId(), HEX);
  String suffix = chipId;
  suffix.toUpperCase();
  while (suffix.length() < 6) {
    suffix = "0" + suffix;
  }

  const String apSsid = "RFID-Setup-" + suffix;
  String apPassword = "password";

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(apSsid.c_str(), apPassword.c_str())) {
    Serial.println(F("Could not start Wi-Fi setup access point."));
    showMessage("Setup AP failed", "Restart device");
    while (true) {
      delay(1000);
    }
  }

  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  webServer.on("/", HTTP_GET, []() {
    webServer.send_P(200, "text/html", CONFIG_PAGE);
  });
  webServer.on("/save", HTTP_POST, []() {
    if (!webServer.hasArg("ssid") || !webServer.hasArg("password")) {
      webServer.send(400, "text/plain", "SSID and password fields are required.");
      return;
    }

    const String ssid = webServer.arg("ssid");
    const String password = webServer.arg("password");
    if (!saveWiFiCredentials(ssid, password)) {
      webServer.send(400, "text/plain",
                     "Invalid Wi-Fi credentials or could not save configuration.");
      return;
    }

    webServer.send(200, "text/html",
                   "<!doctype html><html><body><h3>Saved</h3>"
                   "<p>Restarting. Reconnect to your usual Wi-Fi.</p></body></html>");
    delay(1000);
    ESP.restart();
  });
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.4.1/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();

  Serial.println(F("Wi-Fi setup portal started."));
  Serial.print(F("Connect to: "));
  Serial.println(apSsid);
  Serial.println(F("Open http://192.168.4.1 if the setup page does not appear."));
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Setup WiFi"));
  display.println(apSsid);
  display.print(F("Pass: "));
  display.println(apPassword);
  display.display();

  while (true) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    delay(2);
  }
}

void showMessage(const String& line1, const String& line2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);
  display.println();
  display.println(line2);
  display.display();
}

void beep(uint16_t durationMs = 100) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  showMessage("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected; IP: "));
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println(F("WiFi connection timed out."));
  return false;
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

bool uploadAttendance(const String& uid) {
  if (!connectWiFi()) {
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

  http.setTimeout(12000);
  http.addHeader(F("Content-Type"), F("application/json"));
  http.addHeader(F("apikey"), SUPABASE_ANON_KEY);
  http.addHeader(F("Authorization"), String(F("Bearer ")) + SUPABASE_ANON_KEY);
  http.addHeader(F("Prefer"), F("return=minimal"));

  const String body = String(F("{\"uid\":\"")) + uid +
                      F("\",\"device_id\":\"") + DEVICE_ID + F("\"}");
  const int statusCode = http.POST(body);

  if (statusCode > 0) {
    Serial.printf("Supabase HTTP status: %d\n", statusCode);
  } else {
    Serial.printf("Supabase request failed: %s\n", http.errorToString(statusCode).c_str());
  }
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("SSD1306 initialization failed."));
    while (true) {
      delay(1000);
    }
  }
  showMessage("RFID Attendance", "Starting...");

  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount failed; formatting filesystem."));
    if (!LittleFS.format() || !LittleFS.begin()) {
      Serial.println(F("Could not format or mount LittleFS."));
      showMessage("Storage error", "Check flash layout");
      while (true) {
        delay(1000);
      }
    }
  }

  if (!loadWiFiCredentials() || !connectWiFi()) {
    startConfigPortal();
  }

  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  if (checkRfidConnection()) {
    Serial.println(F("RC522 ready. Scan a card."));
    showMessage("RC522 connected", "Ready to scan");
  } else {
    showMessage("RC522 not found", "Check SPI wiring");
  }
}

void loop() {
#if ENABLE_BATTERY_MONITOR
  static uint32_t lastBatteryRead = 0;
  if (millis() - lastBatteryRead >= 10000) {
    lastBatteryRead = millis();
    const float adcVolts = analogRead(A0) * (ADC_FULL_SCALE_VOLTS / 1023.0f);
    const float batteryVolts =
        adcVolts * (BATTERY_R_TOP_OHMS + BATTERY_R_BOTTOM_OHMS) /
        BATTERY_R_BOTTOM_OHMS;
    Serial.printf("Battery: %.2f V\n", batteryVolts);
  }
#endif

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(20);
    return;
  }

  const String uid = readUid();
  Serial.print(F("Card UID: "));
  Serial.println(uid);
  showMessage("Card scanned", uid);

  const bool uploaded = uploadAttendance(uid);
  if (uploaded) {
    Serial.println(F("Attendance uploaded."));
    showMessage("Attendance saved", uid);
    beep(100);
  } else {
    Serial.println(F("Attendance upload failed."));
    showMessage("Upload failed", "Check WiFi / setup");
    beep(350);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1200);
  showMessage("Ready to scan");
}