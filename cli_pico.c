// This code is adapted for the Raspberry Pi Pico W using the Arduino framework.
// It requires the 'Raspberry Pi Pico/RP2040' maintained by Earle F. Philhower
// library installed via the Library Manager.
// NOTE: The current code uses the high-level Arduino LittleFS wrapper, which is the
// stable and recommended method for file system access in this environment, as the
// low-level lfs.h API requires complex, platform-specific flash driver configuration.

#include <WiFi.h> // Pico W WiFi
#include <WiFiUdp.h>
#include <DHT.h>         // DHT by Adafruit
#include <ArduinoJson.h> // ArduinoJson by Benoit
#include <LittleFS.h>    // Replaces SPIFFS for Pico W
#include <vector>
#include <PubSubClient.h>

// ---------------- CONFIG ----------------
const char *ssid = "Pixel_Alf";
const char *password = "alfredopassword04";

// Note: Using IPAddress type for compatibility with Pico W networking
const IPAddress udp_server_ip(10, 233, 220, 191);
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
const char *DEVICE_ID = "PICO_Device_01";

// --- WIFI CONFIG ---
#define WIFI_CONNECT_TIMEOUT_MS 20000 // 20 seconds max to connect
// ----------------------------

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP udp;

unsigned long seq = 0;    // Sequence number for guaranteed delivery
int qos = 1;              // 0 = best effort, 1 = guaranteed delivery
bool fs_is_ready = false; // Global flag to track successful LittleFS mount state

// Forward declaration
bool sendWithQoS(const String &payload, unsigned long current_seq);
void logDataToFile(const String &payload);
void transmitStoredData();

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

            // Using JsonDocument (best practice)
            JsonDocument doc;
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
    int qos_level = 1;

    // Check for communications failure (Error Handling)
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("ERROR: WiFi disconnected. Cannot send live data.");
        return false; // Communication failure
    }

    do
    {
        // 1. Send UDP packet using the IPAddress object
        udp.beginPacket(udp_server_ip, udp_port);
        udp.print(payload);
        udp.endPacket();

        Serial.print("Sent (Seq ");
        Serial.print(current_seq);
        Serial.print("): ");
        // Only print first 60 chars of payload for logs
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
            // QoS 0 is always "delivered" instantly
            delivered = true;
        }

    } while (!delivered);

    return delivered; // True if ACK received
}

// Function to store a payload string into the LittleFS log file
void logDataToFile(const String &payload)
{
    // Check global flag
    if (!fs_is_ready)
    {
        return;
    }

    // Use "a" (append) mode
    File file = LittleFS.open(log_filepath, "a");
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
    Serial.println("--- Checking for stored telemetry---");

    // Check global flag
    if (!fs_is_ready)
    {
        Serial.println("LittleFS mount failed! Cannot check log.");
        return;
    }

    // Use "r" (read) mode
    File file = LittleFS.open(log_filepath, "r");
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

        // Using JsonDocument
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
            LittleFS.remove(log_filepath);
            Serial.println("All stored messages sent successfully. Log file deleted.");
        }
        else
        {
            // Rewrite the file with only the failed messages
            // Use "w" (write/overwrite) mode
            File new_file = LittleFS.open(log_filepath, "w");
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
    Serial.begin(9600);

    // 1. Initialise LittleFS (REVISED LOGIC: Attempt mount, if fails, format and retry)
    // This process is equivalent to calling lfs_mount and lfs_format.
    if (!LittleFS.begin())
    {
        Serial.println("LittleFS Mount Failed!");
        Serial.println("Attempting to format file system. This will ERASE all files...");

        if (LittleFS.format())
        {
            Serial.println("Formatting successful. Retrying mount...");

            if (LittleFS.begin())
            {
                Serial.println("LittleFS mounted successfully after format.");
                fs_is_ready = true; // Set flag on success
            }
            else
            {
                Serial.println("FATAL: LittleFS still failed to mount after format. Log storage disabled.");
            }
        }
        else
        {
            Serial.println("FATAL: LittleFS formatting failed. Log storage disabled.");
        }
    }
    else
    {
        Serial.println("LittleFS mounted successfully.");
        fs_is_ready = true; // Set flag on success
    }

    // --- Connection Timeout Logic ---

    // Explicitly disconnect before starting a new connection attempt for clean radio state.
    //WiFi.disconnect(true);

    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");

    unsigned long start_time = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_time < WIFI_CONNECT_TIMEOUT_MS)
    {
        Serial.print(".");
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    }
    else
    {
        Serial.println("\nERROR: Failed to connect to WiFi within timeout!");
        // The program will continue. Data will be logged only if fs_is_ready is true.
    }
    // --- END NEW LOGIC ---

    espClient.setInsecure();

    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);

    udp.begin(udp_port);
    dht.begin();

    // 2. Transmit stored data on restart (Req. c)
    // This function now uses the fs_is_ready flag.
    // Moved to loop per request.
    //transmitStoredData();
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
    // Using JsonDocument
    JsonDocument doc;
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
    publishMessage("/comcs/g04/sensor", payload.c_str(), true);

    // 4. Handle failure by logging to file
    if (!delivered)
    {
        // This only happens if max retries failed inside sendWithQoS()
        logDataToFile(payload);
    }else{
      // Attempt to send stored data, since it is expected that the
      // server can now receive and reply
      transmitStoredData();
    }

    // 5. Update sequence number for the next live packet
    // Sequence number increments regardless of logging status, as the logged
    // packet retains its sequence number and will be resent later.
    seq++;

    delay(5000); // Wait 5 seconds before the next observation
}
