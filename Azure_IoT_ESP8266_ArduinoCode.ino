// Necessary libraries
#include <DHT.h>                   // DHT sensor library for temperature and humidity
#include <EEPROM.h>                // EEPROM library for non-volatile storage
#include <ESP8266WiFi.h>           // ESP8266 WiFi functionality
#include <PubSubClient.h>          // MQTT client for Arduino
#include <WiFiClientSecure.h>      // Secure WiFi client
#include <base64.h>                // Base64 encoding/decoding
#include <bearssl/bearssl.h>       // BearSSL cryptography
#include <bearssl/bearssl_hmac.h>  // BearSSl HMAC functions
#include <libb64/cdecode.h>        // Base64 decoding
#include <az_core.h>               // Azure IoT Core SDK
#include <az_iot.h>                // Azure IoT Hub SDK
#include <azure_ca.h>              // Azure CA certificates
#include "iot_configs.h"           // IoT configuration (WIFi, device credentials)
#include <FS.h>                    // SPIFFS for file system storage

// Additional C standard libraries
#include <cstdlib>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// Define constants and configuration
#define AZURE_SDK_CLIENT_USER_AGENT "c%2F" AZ_SDK_VERSION_STRING "(ard;esp8266)"
#define LED_PIN 2                                    // Built-in LED pin (check if it is the same with your ESP)
#define ERROR_LED_PIN 16                             // Built-in ERROR LED pin (check if it is the same with your ESP)
#define DHTPIN 4                                     // DHT sensor data pin (change for yours if you want)
#define DHTTYPE DHT11                                // DHT sensor type
#define MQPIN A0                                     // MQ135 gad sensor ananlog pin (change for yours if you want)
#define EEPROM_SIZE 4096                             // EEPROM maximum size for ESP8266
#define MESSAGE_ID_ADDR 0                            // EEPROM address for message ID storage
#define BUFFER_STATE_ADDR 4                          // EEPROM address for buffer state
#define MAX_MESSAGE_ID 1000000                       // Maximum message ID before reset
#define RAM_BUFFER_SIZE 50                           // Reduced RAM buffer size
#define SPIFFS_BUFFER_SIZE 1000                      // Target number of readings in SPIFFS
#define MAX_PAYLOAD_SIZE 256                         // Maximum MQTT payload size
#define ONE_HOUR_IN_SECS 3600                        // One hour in seconds
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"  // NTP servers for time synchronization
#define MQTT_PACKET_SIZE 1024                        // MQTT packet size
#define RECONNECTION_DELAY 30000                     // 30 seconds between reconnection attempts

// Utility macro for array size calculation
#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))

// Initialize DHT sensor
DHT dht(DHTPIN, DHTTYPE);


// Configuration variables from iot_configs.h
static const char* ssid = IOT_CONFIG_WIFI_SSID;
static const char* password = IOT_CONFIG_WIFI_PASSWORD;
static const char* host = IOT_CONFIG_IOTHUB_FQDN;
static const char* device_id = IOT_CONFIG_DEVICE_ID;
static const char* device_key = IOT_CONFIG_DEVICE_KEY;
static const int port = 8883;  // Default MQTT over SSL port

// Global variables
static WiFiClientSecure wifi_client;
static X509List cert((const char*)ca_pem);  // Azure CA certificate
static PubSubClient mqtt_client(wifi_client);
static az_iot_hub_client client;                       // Azure IoT Hub client
static char sas_token[200];                            // SAS token for authentication
static uint8_t signature[512];                         // Signature buffer
static unsigned char encrypted_signature[32];          // Encrypted signature
static char base64_decoded_device_key[32];             // Base64 decoded device key
static unsigned long next_telemetry_send_time_ms = 0;  // Next telemetry send time
static unsigned long last_reconnection_attempt = 0;    // Last reconnection attempt time
static char telemetry_topic[128];                      // MQTT telemetry topic
String telemetry_payload;                              // Telemetry payload string
static uint32_t telemetry_send_count = 0;              // Telemetry message counter
static unsigned long deviceStartTime = 0;              // Time when device started
static time_t lastKnownTime = 0;                       // Last registered timestamp
static bool timeWasSynchronized = false;               // Time Synchronization with variables

// Sensor data structure for buffering
typedef struct {
  float temperature;
  float humidity;
  float oxygen;
  unsigned long timestamp;
} SensorData;

// Circular buffer for storing sensor data in RAM when offline
SensorData ramDataBuffer[RAM_BUFFER_SIZE];
int ramBufferHead = 0;       // RAM buffer read position
int ramBufferTail = 0;       // RAM buffer write position
int ramBufferCount = 0;      // Number of items in RAM buffer
bool ramBufferFull = false;  // RAM buffer full flag

// SPIFFS buffer management
int spiffsFileCount = 0;      // Number of data files in SPIFFS
int spiffsReadFileIndex = 0;  // Current file being read from SPIFFS
int spiffsReadPosition = 0;   // Current position in the current file

// Last sensor readings
static float lastTemperature = 0;
static float lastHumidity = 0;
static float lastOxygen = 0;

/******************************************************************************
 * Function: connectToWiFi
 * Description: Connects to WiFi using credentials from iot_configs.h
 * Parameters: None
 * Returns: bool - true if connected, false otherwise
 ******************************************************************************/
static bool connectToWiFi() {
  Serial.begin(9600);
  Serial.println();
  Serial.print("Connecting to WIFI SSID ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);

  // Wait for connection with timeout
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print(".");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED while connecting
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("WiFi connected, IP address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(ERROR_LED_PIN, HIGH);  // Turn off ERROR LED
    return true;
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed");
    digitalWrite(ERROR_LED_PIN, LOW);  // Keep ERROR LED on
    return false;
  }
}

/******************************************************************************
 * Function: initializeTime
 * Description: Initializes time using NTP servers
 * Parameters: None
 * Returns: bool - true if succesful, false otherwise
 ******************************************************************************/
static bool initializeTime() {
  Serial.print("Setting time using SNTP");

  // Configure time with NTP servers (UTC-5)
  configTime(-5 * 3600, 0, NTP_SERVERS);

  // Wait for time synchronization with timeout
  unsigned long startTime = millis();
  time_t now = time(NULL);

  while (now < 8 * 365 * 24 * 3600) {  // Check if time is reasonable (after 1978)
    if (millis() - startTime > 10000) {
      Serial.println("failed!");
      return false;
    }
    delay(500);
    Serial.print(".");
    now = time(NULL);
  }

  lastKnownTime = now;
  timeWasSynchronized = true;
  Serial.println("done!");

  Serial.print("Synchronized time: ");
  Serial.println(ctime(&now));

  return true;
}


/******************************************************************************
 * Function: getCurrentLocalTimeString
 * Description: Gets current local time as string
 * Parameters: None
 * Returns: char* - pointer to time string
 ******************************************************************************/
static char* getCurrentLocalTimeString() {
  time_t now = time(NULL);
  return ctime(&now);
}

/******************************************************************************
 * Function: printCurrentTime
 * Description: Prints current time to serial
 * Parameters: None
 * Returns: void
 ******************************************************************************/
static void printCurrentTime() {
  Serial.print("Current time: ");
  Serial.print(getCurrentLocalTimeString());
}

/******************************************************************************
 * Function: receivedCallback
 * Description: MQTT message callback function
 * Parameters: 
 *   - topic: MQTT topic
 *   - payload: Message payload
 *   - length: Payload length
 * Returns: void
 ******************************************************************************/
void receivedCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Received [");
  Serial.print(topic);
  Serial.print("]: ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println("");
}

/******************************************************************************
 * Function: initializeClients
 * Description: Initializes Azure IoT Hub and MQTT clients
 * Parameters: None
 * Returns: bool - true if successful, false otherwise
 ******************************************************************************/
static bool initializeClients() {
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  // Set trust anchors for secure connection
  wifi_client.setTrustAnchors(&cert);

  // Initialize Azure IoT Hub client
  if (az_result_failed(az_iot_hub_client_init(
        &client,
        az_span_create((uint8_t*)host, strlen(host)),
        az_span_create((uint8_t*)device_id, strlen(device_id)),
        &options))) {
    Serial.println("Failed initializing Azure IoT Hub client");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return false;
  }

  // Configure MQTT client
  mqtt_client.setServer(host, port);
  mqtt_client.setCallback(receivedCallback);
  mqtt_client.setBufferSize(MQTT_PACKET_SIZE);

  return true;
}

/******************************************************************************
 * Function: getSecondsSinceEpoch
 * Description: Gets seconds since UNIX epoch
 * Parameters: None
 * Returns: uint32_t - seconds since epoch
 ******************************************************************************/
static uint32_t getSecondsSinceEpoch() {
  return (uint32_t)time(NULL);
}

/******************************************************************************
 * Function: generateSasToken
 * Description: Generates SAS token for Azure IoT Hub authentication
 * Parameters: 
 *   - sas_token: Buffer to store the SAS token
 *   - size: Size of the buffer
 * Returns: int - 0 on success, 1 on failure
 ******************************************************************************/
static int generateSasToken(char* sas_token, size_t size) {
  az_span signature_span = az_span_create((uint8_t*)signature, sizeofarray(signature));
  az_span out_signature_span;
  az_span encrypted_signature_span = az_span_create((uint8_t*)encrypted_signature, sizeofarray(encrypted_signature));

  // Set token expiration (1 hour from now)
  uint32_t expiration = getSecondsSinceEpoch() + ONE_HOUR_IN_SECS;

  // Get SAS signature
  if (az_result_failed(az_iot_hub_client_sas_get_signature(
        &client, expiration, signature_span, &out_signature_span))) {
    Serial.println("Failed getting SAS signature");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }

  // Base64-decode device key
  int base64_decoded_device_key_length = base64_decode_chars(device_key, strlen(device_key), base64_decoded_device_key);

  if (base64_decoded_device_key_length == 0) {
    Serial.println("Failed base64 decoding device key");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }

  // SHA-256 encryption
  br_hmac_key_context kc;
  br_hmac_key_init(
    &kc, &br_sha256_vtable, base64_decoded_device_key, base64_decoded_device_key_length);

  br_hmac_context hmac_ctx;
  br_hmac_init(&hmac_ctx, &kc, 32);
  br_hmac_update(&hmac_ctx, az_span_ptr(out_signature_span), az_span_size(out_signature_span));
  br_hmac_out(&hmac_ctx, encrypted_signature);

  // Base64 encode encrypted signature
  String b64enc_hmacsha256_signature = base64::encode(encrypted_signature, br_hmac_size(&hmac_ctx));

  az_span b64enc_hmacsha256_signature_span = az_span_create(
    (uint8_t*)b64enc_hmacsha256_signature.c_str(), b64enc_hmacsha256_signature.length());

  // URL-encode base64 encoded encrypted signature and generate SAS token
  if (az_result_failed(az_iot_hub_client_sas_get_password(
        &client,
        expiration,
        b64enc_hmacsha256_signature_span,
        AZ_SPAN_EMPTY,
        sas_token,
        size,
        NULL))) {
    Serial.println("Failed getting SAS token");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }

  return 0;
}

/******************************************************************************
 * Function: connectToAzureIoTHub
 * Description: Connects to Azure IoT Hub using MQTT
 * Parameters: None
 * Returns: int - 0 on success, 1 on failure
 ******************************************************************************/
static int connectToAzureIoTHub() {
  size_t client_id_length;
  char mqtt_client_id[128];

  // Get client ID
  if (az_result_failed(az_iot_hub_client_get_client_id(
        &client, mqtt_client_id, sizeof(mqtt_client_id) - 1, &client_id_length))) {
    Serial.println("Failed getting client id");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }

  mqtt_client_id[client_id_length] = '\0';

  char mqtt_username[128];
  // Get MQTT username
  if (az_result_failed(az_iot_hub_client_get_user_name(
        &client, mqtt_username, sizeofarray(mqtt_username), NULL))) {
    printf("Failed to get MQTT clientId, return code\n");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }

  Serial.print("Client ID: ");
  Serial.println(mqtt_client_id);
  Serial.print("Username: ");
  Serial.println(mqtt_username);

  // Attempt connection with timeout
  unsigned long startTime = millis();
  bool connected = false;

  while (!connected && millis() - startTime < 10000) {
    Serial.print("MQTT connecting ... ");

    if (mqtt_client.connect(mqtt_client_id, mqtt_username, sas_token)) {
      Serial.println("connected.");
      connected = true;
    } else {
      Serial.print("failed, status code = ");
      Serial.println(mqtt_client.state());
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
      delay(1000);                       // Wait before retrying
    }
  }

  if (connected) {
    // Subscribe to Cloud-to-Device topic
    mqtt_client.subscribe(AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC);
    digitalWrite(ERROR_LED_PIN, HIGH);  // Turn off ERROR LED
    return 0;
  } else {
    Serial.println("MQTT connection failed after multiple attempts");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return 1;
  }
}

/******************************************************************************
 * Function: initSPIFFS
 * Description: Initializes SPIFFS file system
 * Parameters: None
 * Returns: bool - true if successful, false otherwise
 ******************************************************************************/
bool initSPIFFS() {
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount SPIFFS file system");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return false;
  }

  // Count existing data files
  spiffsFileCount = 0;
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    if (String(dir.fileName()).startsWith("/data_")) {
      spiffsFileCount++;
    }
  }

  Serial.print("Found ");
  Serial.print(spiffsFileCount);
  Serial.println(" data files in SPIFFS");

  return true;
}

/******************************************************************************
 * Function: saveToSPIFFS
 * Description: Saves sensor data to SPIFFS file
 * Parameters:
 *   - data: SensorData to save
 * Returns: bool - true if successful, false otherwise
 ******************************************************************************/
bool saveToSPIFFS(SensorData data) {
  // Create filename with timestamp
  String filename = "/data_" + String(data.timestamp) + ".txt";

  File file = SPIFFS.open(filename, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return false;
  }

  // Write data as CSV: temperature,humidity,oxygen,timestamp
  file.print(data.temperature);
  file.print(",");
  file.print(data.humidity);
  file.print(",");
  file.print(data.oxygen);
  file.print(",");
  file.println(data.timestamp);

  file.close();
  spiffsFileCount++;

  return true;
}

/******************************************************************************
 * Function: readNextFromSPIFFS
 * Description: Reads the next sensor data from SPIFFS
 * Parameters:
 *   - data: Pointer to SensorData to populate
 * Returns: bool - true if data was read, false if no more data
 ******************************************************************************/
bool readNextFromSPIFFS(SensorData* data) {
  // Find the next data file
  String filename;
  Dir dir = SPIFFS.openDir("/");

  while (dir.next()) {
    String currentFile = dir.fileName();
    if (currentFile.startsWith("/data_")) {
      filename = currentFile;
      break;
    }
  }

  if (filename.length() == 0) {
    return false;  // No more files
  }

  // Read the file
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return false;
  }

  // Parse CSV data
  String content = file.readString();
  int firstComma = content.indexOf(',');
  int secondComma = content.indexOf(',', firstComma + 1);
  int thirdComma = content.indexOf(',', secondComma + 1);

  data->temperature = content.substring(0, firstComma).toFloat();
  data->humidity = content.substring(firstComma + 1, secondComma).toFloat();
  data->oxygen = content.substring(secondComma + 1, thirdComma).toFloat();
  data->timestamp = content.substring(thirdComma + 1).toInt();

  file.close();

  // Delete the file after reading
  SPIFFS.remove(filename);
  spiffsFileCount--;

  return true;
}

/******************************************************************************
 * Function: setSensorData
 * Description: Stores sensor data in circular buffer or SPIFFS
 * Parameters:
 *   - temp: Temperature value
 *   - hum: Humidity value
 *   - oxy: Oxygen level value
 * Returns: void
 ******************************************************************************/
void setSensorData(float temp, float hum, float oxy) {
  static time_t lastStoredTime = 0;  // Track the last stored timestamp
  time_t now;

  if (timeWasSynchronized && mqtt_client.connected()) {
    // We have connection and time sync - set current time
    now = time(NULL);
    lastKnownTime = now;  // Update last known time
    lastStoredTime = now;
  } else if (timeWasSynchronized) {
    // Offline but we had time sync before - increment from last stored time
    if (lastStoredTime == 0) {
      // First offline reading, use last known time
      now = lastKnownTime;
    } else {
      // Subsequent offline readings, add 30 seconds
      now = lastStoredTime + 30;
    }
    lastStoredTime = now;
  } else {
    // Never had time sync, use device uptime (fallback)
    now = (time_t)(millis() / 1000);
  }

  SensorData data;
  data.temperature = temp;
  data.humidity = hum;
  data.oxygen = oxy;
  data.timestamp = now;

  // First try to store in RAM buffer
  if (!ramBufferFull) {
    // Store data in RAM buffer
    ramDataBuffer[ramBufferTail] = data;
    ramBufferTail = (ramBufferTail + 1) % RAM_BUFFER_SIZE;
    ramBufferCount++;

    if (ramBufferCount == RAM_BUFFER_SIZE) {
      ramBufferFull = true;
    }

    Serial.println("Data stored in RAM buffer");
  } else {
    // RAM buffer is full, store in SPIFFS
    if (saveToSPIFFS(data)) {
      Serial.println("Data stored in SPIFFS");
    } else {
      Serial.println("Failed to store data in SPIFFS");
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    }
  }
}

/******************************************************************************
 * Function: formatValue
 * Description: Helper function to format values with nan 
 * Parameters:
      - value: Value of the telemetry
 * Returns: String
 ******************************************************************************/
String formatValue(float value) {
  if (isnan(value)) {
    return "null";  // Send null instead of nan
  } else {
    return String(value, 1);
  }
}

/******************************************************************************
 * Function: sendBufferedData
 * Description: Sends all buffered data to Azure IoT Hub
 * Parameters: None
 * Returns: void
 ******************************************************************************/
static void sendBufferedData() {
  bool hasData = (ramBufferCount > 0) || (spiffsFileCount > 0);
  if (!hasData) return;  // Nothing to send

  digitalWrite(LED_PIN, LOW);  // Turn on LED
  Serial.print(millis());
  Serial.print(" ESP8266 Sending telemetry . . . ");

  // Check connection
  if (!mqtt_client.connected()) {
    Serial.println("Failed - not connected to MQTT");
    digitalWrite(LED_PIN, HIGH);       // Turn off LED
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return;
  }

  // Get telemetry topic
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
        &client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL))) {
    Serial.println("Failed az_iot_hub_client_telemetry_get_publish_topic");
    digitalWrite(LED_PIN, HIGH);       // Turn off LED
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    return;
  }

  // First send all data from RAM buffer
  while (ramBufferCount > 0) {
    digitalWrite(LED_PIN, LOW);  // Turn on LED
    SensorData data = ramDataBuffer[ramBufferHead];

    // Create JSON payload
    String buffered_payload = "{ \"messageId\": " + String(telemetry_send_count++) + ", \"deviceId\": \"" + String(device_id) + "\"" + ", \"temperature\": " + formatValue(data.temperature) + ", \"humidity\": " + formatValue(data.humidity) + ", \"oxygen\": " + formatValue(data.oxygen) + ", \"timestamp\":" + String(data.timestamp) + " }";

    // Publish to Azure IoT Hub
    if (mqtt_client.publish(telemetry_topic, buffered_payload.c_str(), false)) {
      Serial.printf("\nSent: %s", buffered_payload.c_str());

      // Update buffer pointers only if publish was successful
      ramBufferHead = (ramBufferHead + 1) % RAM_BUFFER_SIZE;
      ramBufferCount--;

      // Update buffer state
      if (ramBufferFull) {
        ramBufferFull = false;
      }

      saveMessageId();  // Save message ID to EEPROM
    } else {
      Serial.println("Failed to publish message");
      break;  // Stop trying to send if publish fails
    }

    digitalWrite(LED_PIN, HIGH);        // Turn off LED
    digitalWrite(ERROR_LED_PIN, HIGH);  // Turn off ERROR LED
    delay(100);                         // Small delay between messages
  }

  // Then send data from SPIFFS
  while (spiffsFileCount > 0) {
    digitalWrite(LED_PIN, LOW);  // Turn on LED

    SensorData data;
    if (readNextFromSPIFFS(&data)) {
      // Create JSON payload
      String buffered_payload = "{ \"messageId\": " + String(telemetry_send_count++) + ", \"deviceId\": \"" + String(device_id) + "\"" + ", \"temperature\": " + formatValue(data.temperature) + ", \"humidity\": " + formatValue(data.humidity) + ", \"oxygen\": " + formatValue(data.oxygen) + ", \"timestamp\":" + String(data.timestamp) + " }";

      // Publish to Azure IoT Hub
      if (mqtt_client.publish(telemetry_topic, buffered_payload.c_str(), false)) {
        Serial.printf("\nSent from SPIFFS: %s", buffered_payload.c_str());
        saveMessageId();  // Save message ID to EEPROM
      } else {
        Serial.println("Failed to publish message from SPIFFS");
        break;  // Stop trying to send if publish fails
      }

      digitalWrite(LED_PIN, HIGH);        // Turn off LED
      digitalWrite(ERROR_LED_PIN, HIGH);  // Turn off ERROR LED
      delay(100);                         // Small delay between messages
    } else {
      break;  // No more data or error reading
    }
  }

  Serial.println("Finished sending buffered data");
}

/******************************************************************************
 * Function: saveMessageId
 * Description: Saves message ID to EEPROM
 * Parameters: None
 * Returns: void
 ******************************************************************************/
void saveMessageId() {
  // Reset counter to prevent EEPROM wear
  if (telemetry_send_count > MAX_MESSAGE_ID) {
    telemetry_send_count = 0;
  }

  // Save to EEPROM
  EEPROM.put(MESSAGE_ID_ADDR, telemetry_send_count);
  if (!EEPROM.commit()) {
    Serial.println("Failed to save message ID to EEPROM");
    digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
  }
}

/******************************************************************************
 * Function: establishConnection
 * Description: Establishes complete connection to Azure IoT Hub
 * Parameters: None
 * Returns: bool - true if connected, false otherwise
 ******************************************************************************/
static bool establishConnection() {
  // Only try to reconnect every 30 seconds
  if (millis() - last_reconnection_attempt < RECONNECTION_DELAY && last_reconnection_attempt != 0) {
    return false;
  }

  last_reconnection_attempt = millis();

  Serial.println("Attempting to establish connection...");

  if (!connectToWiFi()) {
    Serial.println("WiFi connection failed");
    return false;
  }

  if (!initializeTime()) {
    Serial.println("Time synchronization failed");
    return false;
  }

  printCurrentTime();

  if (!initializeClients()) {
    Serial.println("Client initialization failed");
    return false;
  }

  // Generate SAS token (valid for 1 hour)
  if (generateSasToken(sas_token, sizeofarray(sas_token)) != 0) {
    Serial.println("Failed generating MQTT password");
    return false;
  }

  // Connect to Azure IoT Hub
  if (connectToAzureIoTHub() != 0) {
    Serial.println("Azure IoT Hub connection failed");
    return false;
  }

  Serial.println("Connection established successfully");
  digitalWrite(LED_PIN, LOW);  // Turn on LED
  return true;
}

/******************************************************************************
 * Function: setup
 * Description: Arduino setup function (runs once at startup)
 * Parameters: None
 * Returns: void
 ******************************************************************************/
void setup() {
  // Initialize time at start
  deviceStartTime = millis();

  pinMode(LED_PIN, OUTPUT);           // Configure LED pin
  digitalWrite(LED_PIN, HIGH);        // Turn off LED
  pinMode(ERROR_LED_PIN, OUTPUT);     // Configure ERROR LED pin
  digitalWrite(ERROR_LED_PIN, HIGH);  // Turn off ERROR LED

  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // Load last message ID from EEPROM
  EEPROM.get(MESSAGE_ID_ADDR, telemetry_send_count);
  Serial.print("Loaded message ID from EEPROM: ");
  Serial.println(telemetry_send_count);

  // Initialize DHT sensor
  dht.begin();

  // Initialize SPIFFS
  if (!initSPIFFS()) {
    Serial.println("SPIFFS initialization failed, using RAM only");
  }

  // Establish connection to Azure IoT Hub
  establishConnection();
}

/******************************************************************************
 * Function: loop
 * Description: Arduino main loop function (runs repeatedly)
 * Parameters: None
 * Returns: void
 ******************************************************************************/
void loop() {

  // Check if connected, reconnect if needed (every 30 seconds)
  if (!mqtt_client.connected()) {
    establishConnection();
  }

  // Check if it's time to send telemetry
  if (millis() > next_telemetry_send_time_ms) {

    // Read sensor values
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    float oxygen = analogRead(MQPIN);  // Read MQ135 sensor

    // Validate sensor readings
    if (isnan(temperature)) {
      Serial.println("Failed to read temperature from DHT11!");
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
      temperature = NAN;
    }
    if (isnan(humidity)) {
      Serial.println("Failed to read humidity from DHT11!");
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
      humidity = NAN;
    }
    if (isnan(oxygen)) {
      Serial.println("Failed to read oxygen level from MQ135!");
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
      oxygen = NAN;
    }

    // Store sensor data
    setSensorData(temperature, humidity, oxygen);

    // Send data if connected, otherwise it will be buffered
    if (mqtt_client.connected()) {
      sendBufferedData();
    } else {
      Serial.println("Storing data in buffer (connection lost)");
      digitalWrite(ERROR_LED_PIN, LOW);  // Turn on ERROR LED
    }

    // Schedule next transmission
    next_telemetry_send_time_ms = millis() + TELEMETRY_FREQUENCY_MILLISECS;
  }

  // Process MQTT messages
  mqtt_client.loop();
}
