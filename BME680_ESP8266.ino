#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <Adafruit_BME680.h>

#if defined(ESP8266)

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#define SCL D1
#define SDA D2

#elif defined (ESP32)

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>

// TODO: check pin assignments
#define SCL 7
#define SDA 8

#endif


#define LED_TOGGLE_PERIOD_MS 500
#define BME_SAMPLE_PERIOD_MS (1000L)
// interval is expressed as a multiple of BME_SAMPLE_PERIOD_MS
#define BME_DEFAULT_HISTORY_INTERVAL 10
#define BME_HISTORY_LEN 1000
#define INVALID_TEMPERATURE -999

struct bmeSample_s {
  float temperature;
  float pressure;
  float humidity;
  float gas;
};

static struct bmeSample_s bmeHistory[BME_HISTORY_LEN];
static unsigned int bmeHistoryIndex = 0;
static unsigned int bmeHistoryCount = 0;
static unsigned int bmeHistoryInterval = BME_DEFAULT_HISTORY_INTERVAL;

#define MIME_JSON "application/json"

#define SEALEVELPRESSURE_HPA (1013.25)

#if defined(ESP8266)
ESP8266WebServer server(80);
#elif defined (ESP32)
WebServer server(80);
#endif


#define SEND_BUFFER_LEN 1024
static String sendBuffer = String();

Adafruit_BME680 bme;
bool bmeActive = false;

struct mimeMap_s {
  const char* extension;
  const char* mimeType;
};

String getEnvironmentData() {
  StaticJsonBuffer<128> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["temp"] = bme.temperature;
  root["pressure"] = bme.pressure/100.0;
  root["humidity"] = bme.humidity;
  root["gas"] = bme.gas_resistance / 1000.0;
  sendBuffer = "";
  root.printTo(sendBuffer);
  return sendBuffer;
}

// serve a json string containing the environment data
void serveEnvironmentData() {
  server.send(200, MIME_JSON, getEnvironmentData());
}

void serveHistory() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, MIME_JSON, "");
  server.sendContent("{\"history\":[");
  int toSend = bmeHistoryCount;
  int i = (bmeHistoryIndex - 1) % BME_HISTORY_LEN;
  while (toSend > 0) {
    toSend -= 1;
    StaticJsonBuffer<128> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["temp"] = bmeHistory[i].temperature;
    root["pressure"] = bmeHistory[i].pressure/100.0;
    root["humidity"] = bmeHistory[i].humidity;
    root["gas"] = bmeHistory[i].gas / 1000.0;
    sendBuffer = "";
    root.printTo(sendBuffer);
    if (toSend > 0) {
      sendBuffer += ",";
    }
    server.sendContent(sendBuffer);
    i = (i - 1) % BME_HISTORY_LEN;
  }
  sendBuffer = "],\"interval\":";
  sendBuffer += BME_SAMPLE_PERIOD_MS * bmeHistoryInterval;
  sendBuffer += "}";
  server.sendContent(sendBuffer);
}

void serveStatus() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["ipaddr"] = WiFi.localIP().toString();
  JsonArray& props = root.createNestedArray("status");
  JsonObject& uptime = props.createNestedObject();
  uptime["name"] = "uptime";
  // TODO: handle millis overflow
  uptime["value"] = millis() / 1000;
  uptime["type"] = "seconds";
  sendBuffer = "";
  root.printTo(sendBuffer);
  server.send(200, MIME_JSON, sendBuffer);
}

void serveConfig() {
  StaticJsonBuffer<128> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["historyInterval"] = bmeHistoryInterval;
  sendBuffer = "";
  root.printTo(sendBuffer);
  server.send(200, MIME_JSON, sendBuffer);
}

void setup() {
  struct rst_info *resetInfo = ESP.getResetInfoPtr(); 
  sendBuffer.reserve(SEND_BUFFER_LEN);

  // initialize serial port
  Serial.begin(115200);
  Serial.println("\nReboot");

  if (resetInfo->reason != REASON_DEEP_SLEEP_AWAKE) {
    Serial.println("Initiating deep sleep");
    ESP.deepSleep(ESP.deepSleepMax());
  }

  // initialize (software) I2C bus for the given pins
  Wire.begin(SDA, SCL);

  // switch the pin for the built-in LED to output
  pinMode(LED_BUILTIN, OUTPUT);

  // try to initialize the BME680 and remember if it worked
  if (!bme.begin()) {
    Serial.println("Could not find a valid BME680 sensor, check wiring!");
  } else {
    bmeActive = true;
  }

  // set up BME680 operating parameters if init was successful
  if (bmeActive) {
    // Set up oversampling and filter initialization
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150); // 320*C for 150 ms
  }

#if defined(ESP8266)
  WiFiManager wifiManager;
  // connect to WiFi
  if (!wifiManager.autoConnect()) {
    Serial.println("WiFi autoconnect timed out; rebooting");
    ESP.restart();
    delay(1000);
  }
#elif defined(ESP32)
  // TODO: WiFi initialization with hardcoded password?
#endif

  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  // initialize the over-the-air update server
  ArduinoOTA.begin();

  // initialize the flash file system
  SPIFFS.begin();

  // configure functions to handle specific URLs
  server.on("/api/status", serveStatus);
  server.on("/api/env", serveEnvironmentData);
  server.on("/api/history", serveHistory);
  server.on("/api/config", serveConfig);
  server.serveStatic("/", SPIFFS, "/");
  // start the web server
  server.begin();
}

void loop() {
  // remember the last time the LED was toggled
  static unsigned long lastLedToggle = 0;
  // remember the last time a measurement was made
  static unsigned long lastMeasure = 0;
  // remember number of measurements since last history entry
  static unsigned int measurementCount = 0;

  // let the web server handle outstanding requests
  server.handleClient();
  // let the over-the-air update server handle update requests
  ArduinoOTA.handle();

  unsigned long now = millis();
  
  // toggle the LED if it is due
  if ((now - lastLedToggle) > LED_TOGGLE_PERIOD_MS) {
    lastLedToggle = now;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  // trigger a measurement if one is due
  if ((now - lastMeasure) > BME_SAMPLE_PERIOD_MS) {
    struct bmeSample_s* entry = &bmeHistory[bmeHistoryIndex];
    lastMeasure = now;
    if (bmeActive && bme.performReading()) {
      Serial.println(getEnvironmentData());
      entry->temperature = bme.temperature;
      entry->pressure = bme.pressure;
      entry->humidity = bme.humidity;
      entry->gas = bme.gas_resistance;
    } else {
      Serial.println("Failed to perform reading :(");
      entry->temperature = INVALID_TEMPERATURE;
    }
    if (++measurementCount >= bmeHistoryInterval) {
      measurementCount = 0;
      bmeHistoryIndex = (bmeHistoryIndex + 1) % BME_HISTORY_LEN;
      if (bmeHistoryCount < BME_HISTORY_LEN) {
        bmeHistoryCount += 1;
      }
    }
  }
  delay(100);
}
