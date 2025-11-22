#include <WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>
#include <ArduinoJson.h> // Ensure this is installed

// ---------------- CONFIG ----------------
const char* ssid = "Pixel_Alf";
const char* password = "alfredopassword04";

const char* udp_server_ip = "10.221.195.150";
const int udp_port = 5005;

#define DHTPIN 4
#define DHTTYPE DHT11

// --- NEW CONFIG FOR RETRY ---
#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 200
// ----------------------------

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP udp;

unsigned long seq = 0;      // Sequence number for guaranteed delivery
int qos = 1;                // 0 = best effort, 1 = guaranteed delivery

void setup() {
  Serial.begin(9600);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nConnected!");

  udp.begin(udp_port);
  dht.begin();
}

bool waitForAck(unsigned long mySeq, const char* myId) {
  unsigned long start = millis();
  char incoming[512];
  
  // Note: The ACK timeout is still 200ms. Keep it brief.

  while (millis() - start < 200) {   
    int packetSize = udp.parsePacket();
    if (packetSize) {
      int len = udp.read(incoming, sizeof(incoming) - 1);
      incoming[len] = 0;

      StaticJsonDocument<128> doc; 
      DeserializationError error = deserializeJson(doc, incoming);

      if (error) {
        // Serial.print("ACK Parsing failed: "); // Keep this silent unless debugging
        // Serial.println(error.c_str());
        continue; 
      }

      const char* type = doc["type"] | "";
      const char* id = doc["id"] | "";
      unsigned long seq = doc["seq"] | 0; // Use 0 as default if not found

      if (strcmp(type, "ACK") == 0 &&
          strcmp(id, myId) == 0 &&
          seq == mySeq) {
        
        Serial.println("ACK received!");
        return true;
      }
    }
  }
  return false;
}

void loop() {
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("Failed to read from DHT11!");
    delay(1000);
    return;
  }

  // Build JSON
  String payload = "{"
   "\"id\":\"ESP32_Device_01\","
   "\"type\":\"WeatherObserved\","
   "\"temperature\":" + String(temp, 2) + ","
   "\"relativeHumidity\":" + String(hum, 2) + ","
   "\"dateObserved\":\"" + String(millis()) + "\","
   "\"qos\":" + String(qos) + ","
   "\"seq\":" + String(seq) +
   "}";

  bool delivered = false;
  int retry_count = 0;
  unsigned long backoff_ms = INITIAL_BACKOFF_MS;

  do {
    // 1. Send UDP packet
    udp.beginPacket(udp_server_ip, udp_port);
    udp.print(payload);
    udp.endPacket();

    Serial.println("Sent: " + payload);

    if (qos == 0) break;   // Best effort → no ACK expected

    // 2. Wait for ACK
    delivered = waitForAck(seq, "ESP32_Device_01");

    if (!delivered) {
      retry_count++;
      Serial.print("No ACK -> retrying (Retry #");
      Serial.print(retry_count);
      Serial.print(", Wait ");
      Serial.print(backoff_ms);
      Serial.println("ms)...");
      
      if (retry_count >= MAX_RETRIES) {
        Serial.println("ERROR: Max retries reached! Dropping packet.");
        break; // Exit the do-while loop, packet is lost
      }
      
      // 3. Implement Exponential Backoff
      delay(backoff_ms);
      // Double the backoff delay for the next retry
      backoff_ms *= 2; 
      // Optional: Cap backoff to prevent excessively long delays, e.g., 5000 ms
      if (backoff_ms > 5000) backoff_ms = 5000;
    }

  } while (!delivered);
  
  // Only increment sequence number if delivered OR max retries reached (packet dropped)
  if (delivered || retry_count >= MAX_RETRIES) {
      seq++;   // Next sequence number
  }

  delay(1000); // Base delay between different observations
}
