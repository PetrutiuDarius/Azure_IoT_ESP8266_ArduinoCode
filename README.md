# ESP8266 → Azure IoT Hub — Temperature, Humidity & Gas Monitoring

This project demonstrates how to connect a **NodeMCU 1.0 (ESP-12E / ESP8266)** to **Azure IoT Hub**, read a **DHT11 sensor** (temperature + humidity) and **MQ-135 sensor** (gas / air quality), buffer data during connectivity interruptions, and reliably send data when the connection is available.

---

## 🧰 Table of Contents

1. [Prerequisites](#1-prerequisites)  
2. [Azure IoT Hub & Device Setup](#2-azure-iot-hub--device-setup)  
3. [Wiring / Hardware Connections](#3-wiring--hardware-connections)  
4. [Project Configuration](#4-project-configuration-what-to-change)  
5. [How It Works](#5-how-it-works--high-level)  
6. [Code Features & Behavior](#6-code-features--behavior-detailed)  
7. [Troubleshooting & Tips](#7-troubleshooting--tips)  
8. [References & Further Reading](#8-references--further-reading)

---

## 1. Prerequisites

- Arduino IDE (with ESP8266 board support installed)  
- Basic knowledge of Arduino programming  
- Azure subscription (free tier works)  
- NodeMCU 1.0 (ESP-12E)  
- DHT11 temperature & humidity sensor  
- MQ-135 gas sensor  
- Breadboard and jumper wires  

---

## 2. Azure IoT Hub & Device Setup

1. Go to the Azure portal → **Create a resource → IoT Hub**.  
   - Choose subscription, resource group, region, and hub name.  
   - Select a pricing tier (F1 Free or S1).  

2. After deployment, open your IoT Hub → **IoT Devices** → **+ Add Device**.  
   - Give it a **Device ID** (e.g. `esp8266`).  
   - Authentication type: **Symmetric key**.  
   - Save the device and copy the **Primary key**.  

3. From **Settings → Shared access policies → iothubowner**, copy your **IoT Hub Host Name** (e.g. `yourhub.azure-devices.net`).

You now have:
- `IOT_HUB_HOST_NAME`  
- `DEVICE_ID`  
- `DEVICE_PRIMARY_KEY`

---

## 3. Wiring / Hardware Connections

| Component             | NodeMCU Pin           | Notes |
| --------------------- | --------------------- | ----- |
| DHT11 data pin        | D4 (GPIO 2)           | Adjust `#define DHTPIN` if needed |
| DHT11 VCC             | 3.3V                  | Ensure sensor voltage compatibility |
| DHT11 GND             | GND                   | — |
| MQ-135 analog output  | A0                    | `#define MQPIN A0` |
| LEDs (status/error)   | GPIO 2, GPIO 16       | Built-in or external LEDs |

> ℹ️ Adjust pin definitions in your `.ino` file if your board’s labeling differs.

📘 Tutorials:  
- [Using DHT11 with Arduino (Project Hub)](https://projecthub.arduino.cc/arcaegecengiz/using-dht11-12f621)  
- [MQ-135 Gas Sensor with Arduino (MicrocontrollersLab)](https://microcontrollerslab.com/interfacing-mq-135-gas-sensor-arduino/)

---

## 4. Project Configuration (What to Change)

Open `iot_configs.h` and set:

```c
#define IOT_CONFIG_WIFI_SSID "YOUR_WIFI_SSID"
#define IOT_CONFIG_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define IOT_CONFIG_IOTHUB_FQDN "YOUR_IOT_HUB_HOST_NAME"
#define IOT_CONFIG_DEVICE_ID "YOUR_DEVICE_ID"
#define IOT_CONFIG_DEVICE_KEY "YOUR_DEVICE_PRIMARY_KEY"

#define TELEMETRY_FREQUENCY_MILLISECS 30000
```
* **SSID / Password** → your WiFi network
* **IOT_HUB_FQDN** → the host name (e.g. `myhub.azure-devices.net`)
* **DEVICE_ID** → the device you created in Azure
* **DEVICE_KEY** → the primary key you copied

Then, in your `.ino` file:

* Check and adjust `#define DHTPIN`, `#define LED_PIN`, `#define ERROR_LED_PIN`, and `#define MQPIN` if your wiring is different.
* Verify `TELEMETRY_FREQUENCY_MILLISECS` (how often you send new sensor readings).

---

## 5. How It Works — High Level

Here’s a simplified flow:

1. On **startup** (`setup()`), the board:

   * Initializes WiFi, LEDs, EEPROM, SPIFFS, DHT sensor
   * Loads the last saved message ID from EEPROM
   * Tries to establish a connection to Azure IoT Hub (WiFi → time sync → IoT client init → SAS token → MQTT connect)

2. In the **main loop**:

   * If MQTT is disconnected, try to re-establish connection periodically
   * When it’s time to send a telemetry reading (per your frequency):

     * Read the sensors (DHT11, MQ-135)
     * Validate the readings
     * Call `setSensorData(...)` to buffer or store the reading
     * If MQTT is connected, call `sendBufferedData()` to flush buffered data

3. **Buffering logic**:

   * A **RAM circular buffer** (fixed size) holds recent sensor readings when connection is unavailable
   * If the RAM buffer becomes full, excess data is written to **SPIFFS** as files
   * Later, when connection is restored, both RAM buffer data and SPIFFS files are published

4. **Reliable delivery**:

   * Each message includes a **messageId**, incremented per message
   * After publishing, the pointer in the buffer is advanced only on success
   * The messageId is stored (EEPROM) to survive resets
   * This prevents “re-sending” already sent data

5. **Time / Timestamp logic**:

   * If NTP time sync is successful, sensor data uses real timestamp
   * If offline, but time was previously synchronized, the code estimates new timestamps (e.g. +30 seconds)
   * If time was never synced, fallback is using `millis()` uptime

6. **JSON formatting & MQTT**:

   * Each telemetry is formatted as JSON, e.g.:

     ```json
     {
       "messageId": 123,
       "deviceId": "esp-device1",
       "temperature": 23.4,
       "humidity": 45.6,
       "oxygen": 400,
       "timestamp": 1690000000
     }
     ```

   * Sent over MQTT to the Azure IoT Hub telemetry topic

   * The Azure SDK / `az_iot_hub_client` functions handle topic formation, authentication, etc.

---

## 6. Code Features & Behavior (detailed)

Here are some of the key features and elements in your code:

* **WiFi connection**: `connectToWiFi()`

  * Tries to join using SSID & password
  * Has timeout and error LED indicator

* **Time synchronization**: `initializeTime()`

  * Uses NTP servers to get accurate UTC time
  * Marks `timeWasSynchronized = true`

* **Azure IoT client initialization**: `initializeClients()`

  * Sets up the IoT Hub client
  * Prepares the MQTT client with TLS and trust anchors

* **SAS token generation**: `generateSasToken(...)`

  * Uses device key, base64 decoding, HMAC SHA-256
  * Expires after 1 hour
  * Used as MQTT password

* **MQTT connect**: `connectToAzureIoTHub()`

  * Gets the client ID and username from Azure SDK
  * Connects with MQTT over SSL
  * Subscribes to Cloud-to-Device (C2D) topic to receive messages

* **Buffering / Storage**:

  * **RAM buffer**: circular array `ramDataBuffer[...]`
  * `ramBufferHead`, `ramBufferTail`, `ramBufferCount`, `ramBufferFull` track state
  * If full, future readings are stored in **SPIFFS files** via `saveToSPIFFS()`
  * `readNextFromSPIFFS()` reads and deletes SPIFFS files sequentially

* **Sending buffered data**: `sendBufferedData()`

  * Publishes from RAM buffer first, then SPIFFS
  * Uses same JSON template with incremented messageId
  * Only advances buffer pointer on successful publish
  * Saves messageId to EEPROM via `saveMessageId()`

* **setSensorData(...)**:

  * Determines timestamp strategy (real time vs estimated)
  * Attempts RAM buffer first; if full, falls back to SPIFFS

* **EEPROM persistence**:

  * `telemetry_send_count` is saved at address `MESSAGE_ID_ADDR`
  * Prevents message ID reset after reboot

* **Reconnect logic**:

  * `establishConnection()` handles reconnect attempts with delays
  * Ensures WiFi, time, client init, SAS token, and MQTT connect in correct order

* **LED indicators**:

  * `LED_PIN` used to show sending / activity
  * `ERROR_LED_PIN` used to signal errors or communication failures

---

## 7. Troubleshooting & Tips

* Always **verify wiring** (sensor data pins, power, ground).
* Use **Serial output** (e.g. `Serial.println`) to see debug prints and error states.
* If DHT readings return `NaN`, check sensor wiring or try a delay and power adjustments.
* Ensure you have valid time before sending data (otherwise your timestamp logic may misbehave).
* Make sure your device key and IoT Hub host name are correctly typed (typos here lead to auth failures).
* Ensure enough flash storage for SPIFFS usage; test SPIFFS begin in isolation if needed.
* Test with the MQTT publishing alone first (skipping buffering) to validate connectivity.
* Monitor Azure IoT Hub’s *Metrics / Operations logs* to see if your device is connecting or sending telemetry with this command in the azure cli: az iot hub monitor-events --hub-name YOUR_IOT_HUB_NAME --device-id YOUR_DEVICE_ID .

---