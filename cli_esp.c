// This code is adapted for the ESP32 platform.
// It combines guaranteed delivery (QoS) via UDP with publishing to an MQTT broker.
// Required Libraries: WiFi, DHT, ArduinoJson, PubSubClient, SPIFFS

#include <WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>          // DHT by Adafruit
#include <ArduinoJson.h>  // ArduinoJson by Benoit
#include <SPIFFS.h>       // File system for ESP32
#include <vector>
#include <PubSubClient.h> // MQTT client library
#include <WiFiClientSecure.h> // FIX: Included and used for secure MQTT connection (8883)

// ---------------- CONFIG ----------------
const char* ssid = "Pixel_Alf";
const char* password = "alfredopassword04";

// UDP Server Configuration (for QoS Telemetry)
const char *udp_server_ip = "10.233.220.191";
const int udp_port = 5005;

// MQTT Broker Configuration (for Command Centre Alerts/Data)
const char *mqtt_server = "4979254f05ea480283d67c6f0d9f7525.s1.eu.hivemq.cloud";
const char *mqtt_username = "web_client";
const char *mqtt_password = "Password1";
const int mqtt_port = 8883; // Secure MQTT port

#define DHTPIN 4
#define DHTTYPE DHT11

// --- CONFIG FOR RETRY & LOGGING ---
#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 200
const char *log_filepath = "/telemetry_log.txt";
const char *DEVICE_ID = "ESP32_Device_01";
// ----------------------------

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP udp;
// FIX 1: Must use WiFiClientSecure for setInsecure() and port 8883
WiFiClientSecure espClient; 
PubSubClient client(espClient); // MQTT Client using secure WiFiClient

unsigned long seq = 0; // Sequence number for guaranteed delivery
int qos = 1;           // 0 = best effort, 1 = guaranteed delivery

// Forward declaration
bool sendWithQoS(const String &payload, unsigned long current_seq);
void logDataToFile(const String &payload);
void transmitStoredData();
void reconnectMqtt();
// FIX: Updated signature to accept String payload for convenience, converting inside the function
void publishMessage(const char *topic, const String &payload, boolean retained);

// Function to wait for an acknowledgement from the UDP server
bool waitForAck(unsigned long mySeq, const char *myId)
{
    unsigned long start = millis();
    char incoming[512];

    // Check for ACK for a brief timeout (800ms)
    while (millis() - start < 800)
    {
        int packetSize = udp.parsePacket();
        if (packetSize)
        {
            int len = udp.read(incoming, sizeof(incoming) - 1);
            incoming[len] = 0;

            // Use JsonDocument (best practice, though original used StaticJsonDocument)
            // Note: Since this block was not causing the error, I'm keeping the original type for minimal change.
            StaticJsonDocument<128> doc;
            DeserializationError error = deserializeJson(doc, incoming);

            if (error)
            {
                continue;
            }

            const char *type = doc["type"] | "";
            const char *id = doc["id"] | "";
            unsigned long seq = doc["seq"] | 0;

            if (strcmp(type, "ACK") == 0 &&
                strcmp(id, myId) == 0 &&
                seq == mySeq)
            {
                Serial.println("ACK received!");
                return true;
            }
        }
    }
    return false;
}

// Function to handle the actual sending and QoS (ACK/Retry) logic.
// Returns true on successful delivery (ACK received or QoS 0).
bool sendWithQoS(const String &payload, unsigned long current_seq)
{
    bool delivered = false;
    int retry_count = 0;
    unsigned long backoff_ms = INITIAL_BACKOFF_MS;
    int qos_level = 1;

    // Check for communications failure (Error Handling)
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("ERROR: WiFi disconnected. Cannot send live data.");
        return false; // Communication failure
    }

    do
    {
        // 1. Send UDP packet (using const char* IP)
        udp.beginPacket(udp_server_ip, udp_port);
        udp.print(payload);
        udp.endPacket();

        Serial.print("Sent UDP (Seq ");
        Serial.print(current_seq);
        Serial.print("): ");
        Serial.println(payload.substring(0, min((int)payload.length(), 60)) + "...");

        // 2. Wait for ACK (Error Handling)
        if (qos_level == 1)
        {
            delivered = waitForAck(current_seq, DEVICE_ID);

            if (!delivered)
            {
                retry_count++;
                Serial.print("No ACK -> retrying (Retry #");
                Serial.print(retry_count);
                Serial.print(", Wait ");
                Serial.print(backoff_ms);
                Serial.println("ms)...");

                if (retry_count >= MAX_RETRIES)
                {
                    Serial.println("ERROR: Max retries reached! Will log to file.");
                    return false; // Indicate failure to deliver/log
                }

                // 3. Implement Exponential Backoff (Error Handling)
                delay(backoff_ms);
                backoff_ms *= 2;
                if (backoff_ms > 5000)
                    backoff_ms = 5000;
            }
        }
        else
        {
            delivered = true;
        }

    } while (!delivered);

    return delivered; // True if ACK received
}

// Function to store a payload string into the SPIFFS log file
void logDataToFile(const String &payload)
{
    // Check global flag (SPIFFS for ESP32)
    if (!SPIFFS.begin())
    {
        return;
    }

    // Use "a" (append) mode
    File file = SPIFFS.open(log_filepath, "a");
    if (!file)
    {
        Serial.println("Failed to open file for appending.");
        return;
    }

    // Write payload followed by a newline character
    if (file.println(payload) > 0)
    {
        Serial.println("Telemetry successfully logged to file.");
    }
    else
    {
        Serial.println("File write failed!");
    }
    file.close();
}

// Function to transmit stored data on restart/reconnection
void transmitStoredData()
{
    Serial.println("--- Checking for stored telemetry on restart/reconnect ---");

    if (!SPIFFS.begin())
    {
        Serial.println("SPIFFS mount failed! Cannot check log.");
        return;
    }

    // Use "r" (read) mode
    File file = SPIFFS.open(log_filepath, "r");
    if (!file)
    {
        Serial.println("No existing telemetry log file found.");
        return;
    }

    std::vector<String> failed_messages;
    int count = 0;

    // Read stored data line by line
    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();

        if (line.length() == 0)
            continue;

        // Use JsonDocument (best practice)
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, line);

        if (error)
        {
            Serial.println("Error parsing stored JSON. Skipping line.");
            continue;
        }

        unsigned long stored_seq = doc["seq"] | 0;

        // Attempt to send the stored message with QoS/Retries
        if (sendWithQoS(line, stored_seq))
        {
            Serial.println("Stored message successfully re-sent and acknowledged.");
        }
        else
        {
            // Failed to send after max retries -> keep it in the buffer
            failed_messages.push_back(line);
            Serial.println("Failed to re-send. Keeping in log.");
        }
        count++;
    }

    file.close();

    // ----------------------------------------------------------------------
    // Rolling Window Logic: Rewrite the log file with only the failed messages
    // ----------------------------------------------------------------------

    if (count > 0)
    {
        Serial.print("Finished processing ");
        Serial.print(count);
        Serial.println(" stored messages.");

        if (failed_messages.empty())
        {
            // All messages sent successfully, delete the file.
            SPIFFS.remove(log_filepath);
            Serial.println("All stored messages sent successfully. Log file deleted.");
        }
        else
        {
            // Rewrite the file with only the failed messages
            // Use "w" (write/overwrite) mode
            File new_file = SPIFFS.open(log_filepath, "w");
            if (!new_file)
            {
                Serial.println("FATAL: Failed to open file for rewriting!");
                return;
            }

            Serial.print("Rewriting log with ");
            Serial.print(failed_messages.size());
            Serial.println(" messages that failed to re-send.");

            for (const String &msg : failed_messages)
            {
                new_file.println(msg);
            }
            new_file.close();
            Serial.println("Log file rewritten.");
        }
    }
}

//------------------------------
void reconnectMqtt()
{
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection... ");

        String clientId = "ESP32-G04-";
        clientId += String(random(0xffff), HEX);

        // Connect with client ID, username, and password
        if (client.connect(clientId.c_str(), mqtt_username, mqtt_password))
        {
            Serial.println("connected");
            // Subscribe to the command topic
            client.subscribe("/comcs/g04/commands");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" retrying in 5 seconds");
            delay(5000);
        }
    }
}

//------------------------------
void callback(char *topic, byte *payload, unsigned int length)
{
    Serial.print("Command received on topic: ");
    Serial.println(topic);
    Serial.print("Payload: ");
    for (unsigned int i = 0; i < length; i++)
        Serial.print((char)payload[i]);
    Serial.println();
    // Add logic here to process received commands
}

//------------------------------
void publishMessage(const char *topic, const String &payload, boolean retained)
{
    // FIX 2: Use payload.c_str() for publishing
    if (client.publish(topic, payload.c_str(), retained))
    {
        Serial.println("JSON published to " + String(topic));
    } else {
        Serial.print("MQTT publish failed for topic: ");
        Serial.println(topic);
    }
}

void setup()
{
    delay(5000); // Wait for serial monitor to open
    Serial.begin(9600);

    // 1. Initialise SPIFFS
    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS Mount Failed! Cannot meet logging requirement.");
    }
    else
    {
        Serial.println("SPIFFS mounted successfully.");
    }

    // --- Wi-Fi Connection ---
    WiFi.disconnect(true);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    
    unsigned long start_time = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_time < 30000) // 30s timeout
    {
        Serial.print(".");
        delay(500);
    }
    
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nFATAL ERROR: Failed to connect to WiFi!");
    }
    
    // --- MQTT Setup ---
    // The PubSubClient library needs a client context for the server, port, and callback
    // FIX 1: setInsecure() requires the secure client, espClient is now WiFiClientSecure
    espClient.setInsecure(); 
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);

    udp.begin(udp_port);
    dht.begin();

    // 2. Transmit stored data on restart
    transmitStoredData();
}

void loop()
{
    // Maintain MQTT connection
    if (!client.connected())
        reconnectMqtt();

    // Required for the MQTT client to process incoming and outgoing messages
    client.loop();

    // 1. Read sensor data
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();

    if (isnan(temp) || isnan(hum))
    {
        Serial.println("Failed to read from DHT11!");
        delay(1000);
        return;
    }

    // 2. Build JSON payload
    JsonDocument doc; // Fixed: Use JsonDocument (best practice)
    doc["id"] = DEVICE_ID;
    doc["type"] = "WeatherObserved";
    doc["temperature"] = temp;
    doc["relativeHumidity"] = hum;
    doc["dateObserved"] = millis(); // Using internal time for simplicity
    doc["qos"] = qos;
    doc["seq"] = seq;

    String payload;
    serializeJson(doc, payload);

    // 3. Attempt to send with QoS (UDP)
    bool delivered = sendWithQoS(payload, seq);
    
    // 4. Publish via MQTT for command center visibility
    // FIX 2: The publishMessage function is now called correctly
    publishMessage("/comcs/g04/sensor", payload, true); 
    
    // 5. Handle failure by logging to file (Req. a)
    if (!delivered)
    {
        logDataToFile(payload);
    }

    // 6. Update sequence number
    seq++;

    delay(5000); // Wait 5 seconds before the next observation
}
