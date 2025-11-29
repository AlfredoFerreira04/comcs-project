#include <WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>         // DHT by Adafruit
#include <ArduinoJson.h> // ArduinoJson by Benoit
#include <SPIFFS.h>
#include <vector>
#include <PubSubClient.h>

// ---------------- CONFIG ----------------
const char *ssid = "Redmi Note 12 Pro 5G";
const char *password = "barbosa2632004";

const char *udp_server_ip = "XXX.XXX.XXX.XXX";
const int udp_port = 5005;

// MQTT Broker settings
const char *mqtt_server = "4979254f05ea480283d67c6f0d9f7525.s1.eu.hivemq.cloud";
const char *mqtt_username = "web_client";
const char *mqtt_password = "Password1";
const int mqtt_port = 8883;

WiFiClientSecure espClient;
PubSubClient client(espClient);

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

unsigned long seq = 0; // Sequence number for guaranteed delivery
int qos = 1;           // 0 = best effort, 1 = guaranteed delivery

// Forward declaration
bool sendWithQoS(const String &payload, unsigned long current_seq);
void logDataToFile(const String &payload);
void transmitStoredData();

// Function to wait for an acknowledgement from the UDP server
bool waitForAck(unsigned long mySeq, const char *myId)
{
    unsigned long start = millis();
    char incoming[512];

    // Check for ACK for a brief timeout (200ms)
    while (millis() - start < 800)
    {
        int packetSize = udp.parsePacket();
        if (packetSize)
        {
            int len = udp.read(incoming, sizeof(incoming) - 1);
            incoming[len] = 0;

            StaticJsonDocument<128> doc;
            DeserializationError error = deserializeJson(doc, incoming);

            if (error)
            {
                // Serial.print("ACK Parsing failed: ");
                // Serial.println(error.c_str());
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
    int qos_level = 1; // Always assume 1 for this function

    // Check for communications failure (Req. b: Error Handling)
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("ERROR: WiFi disconnected. Cannot send live data.");
        return false; // Communication failure
    }

    do
    {
        // 1. Send UDP packet
        udp.beginPacket(udp_server_ip, udp_port);
        udp.print(payload);
        udp.endPacket();

        Serial.print("Sent (Seq ");
        Serial.print(current_seq);
        Serial.print("): ");
        // Only print first 60 chars of payload for logs
        Serial.println(payload.substring(0, min((int)payload.length(), 60)) + "...");

        // 2. Wait for ACK (Req. b: Error Handling)
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

                // 3. Implement Exponential Backoff (Req. b: Error Handling)
                delay(backoff_ms);
                backoff_ms *= 2;
                if (backoff_ms > 5000)
                    backoff_ms = 5000;
            }
        }
        else
        {
            // QoS 0 is always "delivered" instantly
            delivered = true;
        }

    } while (!delivered);

    return delivered; // True if ACK received
}

// Function to store a payload string into the SPIFFS log file (Req. a)
void logDataToFile(const String &payload)
{
    // Check if SPIFFS is ready (it should be initialized in setup)
    if (!SPIFFS.begin())
    {
        Serial.println("SPIFFS not mounted! Cannot log data.");
        return;
    }

    File file = SPIFFS.open(log_filepath, FILE_APPEND);
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

// Function to transmit stored data on restart/reconnection (Req. c)
void transmitStoredData()
{
    Serial.println("--- Checking for stored telemetry on restart/reconnect ---");

    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS mount failed! Cannot check log.");
        return;
    }

    File file = SPIFFS.open(log_filepath, FILE_READ);
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

        StaticJsonDocument<512> doc;
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
            File new_file = SPIFFS.open(log_filepath, FILE_WRITE);
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
void reconnect()
{
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection... ");

        String clientId = "ESP32-G04-";
        clientId += String(random(0xffff), HEX);

        if (client.connect(clientId.c_str(), mqtt_username, mqtt_password))
        {
            Serial.println("connected");
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
    Serial.print("Message received: ");
    for (int i = 0; i < length; i++)
        Serial.print((char)payload[i]);
    Serial.println();
}

//------------------------------
void publishMessage(const char *topic, const char *payload, boolean retained)
{
    if (client.publish(topic, payload, retained))
    {
        Serial.println("JSON published to " + String(topic));
        Serial.println(payload);
    }
}

void setup()
{
    delay(5000);
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

    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

    espClient.setInsecure();

    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);

    udp.begin(udp_port);
    dht.begin();

    // 2. Transmit stored data on restart (Req. c)
    transmitStoredData();
}

void loop()
{
    if (!client.connected())
        reconnect();

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
    StaticJsonDocument<512> doc;
    doc["id"] = DEVICE_ID;
    doc["type"] = "WeatherObserved";
    doc["temperature"] = temp;
    doc["relativeHumidity"] = hum;
    doc["dateObserved"] = millis(); // Using internal time for simplicity
    doc["qos"] = qos;
    doc["seq"] = seq;

    String payload;
    serializeJson(doc, payload);

    // 3. Attempt to send with QoS
    bool delivered = sendWithQoS(payload, seq);
    publishMessage("/comcs/g04/sensor", payload, true);

    // 4. Handle failure by logging to file (Req. a)
    if (!delivered)
    {
        // This only happens if max retries failed inside sendWithQoS()
        logDataToFile(payload);
    }

    // 5. Update sequence number for the next live packet
    // Sequence number increments regardless of logging status, as the logged
    // packet retains its sequence number and will be resent later.
    seq++;

    delay(5000); // Wait 5 seconds before the next observation
}
