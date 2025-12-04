# IoT Telemetry System (Pico W & ESP32)

This repository contains the client code for both **Raspberry Pi Pico W** (using the Arduino framework) and **ESP32** devices, designed to send sensor telemetry with **Guaranteed Delivery (QoS Level 1)** over **UDP** and simultaneously publish data to a central **MQTT Broker**.

The system incorporates **data persistence (SPIFFS/LittleFS)** for buffering messages during network outages and an **Adaptive Throttling** mechanism to manage congestion at the server.

---

## ⚙️ Project Structure and Components

The project consists of four main components:

1. **Client 1 (Pico W):** Responsible for reading sensor data, sending it via **QoS-UDP**, publishing it via **MQTT**, and logging failed messages to **LittleFS**.
2. **Client 2 (ESP32):** Responsible for reading sensor data, sending it via **QoS-UDP**, publishing it via **MQTT**, and logging failed messages to **SPIFFS**.
3. **Server (Placeholder):** A C-based application that receives UDP packets, sends acknowledgements (ACKs) for QoS, and potentially bridges data to another system. *(To be completed in a later section.)*
4. **Command Centre:** a Node-RED dashboard responsible for subscribing to **MQTT** topics to visualize real-time sensor telemetry, while simultaneously monitoring and displaying critical alerts received from the Alert Server.

---

## 🛠️ Client Setup and Dependencies

The client code uses the **Arduino IDE** for development. Ensure you have the appropriate board packages installed for the ESP32 and Raspberry Pi Pico.

### 1. Core Board Setup

| Component | Board Package/Framework | Key Configuration |
| :--- | :--- | :--- |
| **Pico W** | `arduino-pico` | Select `Raspberry Pi Pico W` board. |
| **ESP32** | `esp32` by Espressif | Select your specific ESP32 board (e.g., `ESP32 Dev Module`). |

### 2. Required Libraries (Arduino/PlatformIO)

The following libraries must be installed via the Arduino Library Manager.

| Library Name | Author | Purpose |
| :--- | :--- | :--- |
| **DHT Sensor Library** | Adafruit | Interfaces with the **DHT11/DHT22** temperature/humidity sensor. |
| **ArduinoJson** | Benoit Blanchon | Efficiently constructs and parses the **JSON** telemetry payload. |
| **PubSubClient** | Nick O'Leary | Handles the **MQTT** (Message Queuing Telemetry Transport) client connection. |
| **WiFiClientSecure** | (Included in board package) | Enables **secure connections (TLS/SSL)** for MQTT on port **8883**. |
| **LittleFS** | (Pico W board package) | File system for persistent data logging on the **Pico W**. |
| **SPIFFS** | (ESP32 board package) | File system for persistent data logging on the **ESP32**. |

### 3. Hardware Dependencies

* **Microcontroller:** Raspberry Pi Pico W or ESP32 board.
* **Sensor:** **DHT11** or **DHT22** digital temperature/humidity sensor.
* **Wiring:** Connect the DHT sensor's data pin to the specified `DHTPIN` (e.g., **GPIO 4** in the provided code).

---

## 🔑 Client Configuration

Before compiling and uploading the code, you must update the configuration constants for your local network and MQTT broker.

| Constant | Value (Example from Code) | Description |
| :--- | :--- | :--- |
| `ssid` | `"Pixel_Alf"` | **Your Wi-Fi Network Name.** |
| `password` | `"alfredopassword04"` | **Your Wi-Fi Network Password.** |
| `udp_server_ip` | `10.233.220.191` | **The IP Address of your C-based UDP Server.** |
| `udp_port` | `5005` | The port the UDP Server is listening on. |
| `mqtt_server` | `4979...hivemq.cloud` | MQTT Broker Address. |
| `mqtt_username` | `"web_client"` | Your MQTT connection username. |
| `mqtt_password` | `"Password1"` | Your MQTT connection password. |
| `mqtt_port` | `8883` | MQTT Port. |
| `DEVICE_ID` | `"PICO_Device_01"` / `"ESP32_Device_01"` | A unique identifier for the device (used in QoS ACK). |

---

## 💻 Server Setup: C-Based Alert/QoS UDP Server

This server application is designed to run on a **Linux machine** and acts as the central hub for receiving UDP telemetry, ensuring Guaranteed Delivery (QoS), logging events, and pushing critical alerts via a secure MQTT broker.

### 1. ⚙️ Server Responsibilities and Features

The server executes the following critical functions, corresponding to the requirements outlined in the C code:

| Requirement | Feature | Description |
| :--- | :--- | :--- |
| **Req 2a** | **UDP Binding** | Binds to port `5005` on all interfaces (`INADDR_ANY`). |
| **Req 2b** | **Guaranteed Delivery (QoS-1)** | Detects and ignores **duplicate packets** by tracking the `seq` number for each device. Sends a JSON **ACK** packet back to the client via UDP upon successful, non-duplicate receipt. |
| **Req 2c** | **Device Management** | Tracks the state (ID, last reading, network address, `last_seen` timestamp, last `seq` number) for up to `1024` devices using the `device_t` structure. |
| **Req 2d** | **Range Validation/Logging** | Validates `temperature` and `relativeHumidity` against defined `MIN/MAX` ranges (e.g., $0-50^\circ\text{C}$). Logs all critical events to `stdout` and a persistent file (`alerts.log`). |
| **Req 2e** | **Differential Calculation** | Performs a **differential check** by comparing the new reading against the last recorded readings of **all other connected devices**. Triggers a `DIFFERENTIAL_ALERT` if thresholds (e.g., $3.0^\circ\text{C}$, $20.0\%$) are exceeded. |
| **Req 2f** | **MQTT Alert Publishing** | Publishes all generated alerts (Range/Differential/Inactivity) as structured JSON messages to the secure MQTT topic `/comcs/g04/alerts`. |
| **NEW** | **Client Inactivity Monitor** | A dedicated `pthread` checks if a client's `last_seen` timestamp is older than `INACTIVITY_TIMEOUT_SEC` (10 seconds) and triggers a `CLIENT_INACTIVITY` alert. |

---

### 2. 📚 Dependencies and Compilation

This application relies on standard Linux libraries for networking, along with two specific third-party C libraries.

#### Required Libraries

| Library | Linux Package (Common) | Purpose |
| :--- | :--- | :--- |
| **cJSON** | `libcjson-dev` | Lightweight C library for JSON parsing (used for telemetry/alert/ACK building). |
| **Paho MQTT C Client** | `libpaho-mqtt3c-dev` | Secure MQTT client for publishing alerts over SSL/TLS. |
| **pthreads** | (Standard Library) | POSIX threads for background tasks (MQTT Keep-Alive, Device Monitoring). |
| **libm** | (Standard Library) | The math library, specifically for the `fabs()` function (for absolute difference in differential check). |

#### Installation on Debian/Ubuntu

Install the required packages using your system's package manager:

```bash
sudo apt update
sudo apt install build-essential git
# Install cJSON and Paho MQTT development libraries
sudo apt install libcjson-dev libpaho-mqtt-dev
```

#### Running the Server

To run the server simply use the interface provided by the Makefile

```Makefile
server:
 gcc srv.c -o server -lpaho-mqtt3cs -lcjson

clean:
 rm server *.log

run:
 ./server
```
