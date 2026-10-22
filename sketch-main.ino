#include <WiFiS3.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASS[] = "YOUR_WIFI_PASSWORD";
const char WRITE_API_KEY[] = "YOUR_THINGSPEAK_WRITE_KEY";

const char SERVER[] = "api.thingspeak.com";
const int HTTPS_PORT = 443;
const int MAX_ATTEMPTS = 3;

WiFiSSLClient client;

// UART input buffer
char lineBuffer[120];
size_t lineLength = 0;
bool discardLine = false;

// Latest validated measurements
float readings[8];
bool haveData = false;

unsigned long lastUpload = 0;
unsigned long lastReceived = 0;

const unsigned long UPLOAD_INTERVAL = 20000;
const unsigned long DATA_TIMEOUT = 10000;

// --------------------
// Validate CSV packet
// --------------------
bool parsePacket(char* line) {
  float temporary[8];
  char* cursor = line;

  for (int i = 0; i < 8; i++) {
    char* endPointer;

    temporary[i] = strtod(cursor, &endPointer);

    if (endPointer == cursor || !isfinite(temporary[i])) {
      return false;
    }

    if (i < 7) {
      if (*endPointer != ',') return false;
      cursor = endPointer + 1;
    } else {
      if (*endPointer != '\0') return false;
    }
  }

  // Basic range and state checks
  if (temporary[0] < 0 || temporary[0] > 1023) return false;
  if (temporary[1] != 0 && temporary[1] != 1) return false;
  if (temporary[2] < 0 || temporary[2] > 1023) return false;
  if (temporary[4] < 0 || temporary[4] > 100) return false;
  if (temporary[5] < 300 || temporary[5] > 1200) return false;
  if (temporary[6] < 0 || temporary[6] > 20) return false;
  if (temporary[7] != 0 && temporary[7] != 1) return false;

  for (int i = 0; i < 8; i++) {
    readings[i] = temporary[i];
  }

  haveData = true;
  lastReceived = millis();

  return true;
}

// --------------------
// Wi-Fi connection
// --------------------
void connectWiFi() {
  int attempts = 0;
  int status = WiFi.status();

  while (status != WL_CONNECTED && attempts < MAX_ATTEMPTS) {
    Serial.print("Attempt ");
    Serial.print(attempts + 1);
    Serial.print(" of ");
    Serial.println(MAX_ATTEMPTS);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    delay(5000);

    status = WiFi.status();
    attempts++;
  }

  if (status == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("SSID: ");
    Serial.println(WIFI_SSID);
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(
      "Failed to connect after multiple attempts. "
      "Will retry on the next upload."
    );
  }
}

// --------------------
// Upload to ThingSpeak
// --------------------
void uploadThingSpeak() {
  if (!haveData) return;

  // Do not upload stale measurements
  if (millis() - lastReceived > DATA_TIMEOUT) {
    Serial.println("Data stale: upload skipped");
    return;
  }

  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload skipped: WiFi disconnected");
    return;
  }

  // Recheck freshness after the connection attempt
  if (millis() - lastReceived > DATA_TIMEOUT) {
    Serial.println("Data stale after WiFi connection: upload skipped");
    return;
  }

  char body[300];

  int length = snprintf(
    body, sizeof(body),
    "api_key=%s"
    "&field1=%.0f"
    "&field2=%.0f"
    "&field3=%.0f"
    "&field4=%.2f"
    "&field5=%.2f"
    "&field6=%.2f"
    "&field7=%.3f"
    "&field8=%.0f",
    WRITE_API_KEY,
    readings[0],
    readings[1],
    readings[2],
    readings[3],
    readings[4],
    readings[5],
    readings[6],
    readings[7]
  );

  if (length <= 0 || length >= (int)sizeof(body)) {
    Serial.println("Request body error");
    return;
  }

  Serial.println("Connecting to ThingSpeak...");

  if (!client.connect(SERVER, HTTPS_PORT)) {
    Serial.println("HTTPS connection failed");
    return;
  }

  client.println("POST /update HTTP/1.1");
  client.println("Host: api.thingspeak.com");
  client.println(
    "Content-Type: application/x-www-form-urlencoded"
  );
  client.print("Content-Length: ");
  client.println(length);
  client.println("Connection: close");
  client.println();
  client.print(body);

  // Read response with inactivity timeout
  unsigned long start = millis();
  char response[500];
  size_t used = 0;

  while (millis() - start < 7000) {
    while (client.available()) {
      char c = client.read();

      if (used < sizeof(response) - 1) {
        response[used++] = c;
      }

      start = millis();
    }

    if (!client.connected() && !client.available()) {
      break;
    }
  }

  response[used] = '\0';
  client.stop();

  Serial.println("Server response:");
  Serial.println(response);

  if (strstr(response, "200 OK") != NULL) {
    Serial.println("HTTP accepted; verify ThingSpeak entry ID");
  } else {
    Serial.println("Upload not confirmed");
  }
}

// --------------------
// Setup
// --------------------
void setup() {
  Serial.begin(115200);

  // Grove TX -> UNO R4 RX (D0)
  // Grove RX -> UNO R4 TX (D1)
  Serial1.begin(9600);

  delay(1000);

  Serial.println("IoT LAB GATEWAY");
  connectWiFi();
}

// --------------------
// Main loop
// --------------------
void loop() {
  while (Serial1.available()) {
    char c = Serial1.read();

    if (c == '\n') {
      if (!discardLine) {
        lineBuffer[lineLength] = '\0';

        if (parsePacket(lineBuffer)) {
          Serial.print("Valid UART packet: ");
          Serial.println(lineBuffer);
        } else {
          Serial.println("Invalid UART packet");
        }
      }

      lineLength = 0;
      discardLine = false;
    } else if (c != '\r' && !discardLine) {
      if (lineLength < sizeof(lineBuffer) - 1) {
        lineBuffer[lineLength++] = c;
      } else {
        lineLength = 0;
        discardLine = true;
        Serial.println("UART buffer overflow: discarding line");
      }
    }
  }

  if (millis() - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = millis();
    uploadThingSpeak();
  }
}