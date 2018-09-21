#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <Adafruit_BME680.h>
#include <MD5Builder.h>
#include <EEPROM.h>
#include <time.h>

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
  time_t time;
  float temperature;
  float pressure;
  float humidity;
  float gas;
};

static struct bmeSample_s bmeHistory[BME_HISTORY_LEN];
static unsigned int bmeHistoryIndex = 0;
static unsigned int bmeHistoryCount = 0;
// remember number of measurements since last history entry
static unsigned int measurementCount = 0;
// UTC time of last measurement
static time_t sampleTime;

static struct configValues_s {
    unsigned int historyInterval;
    bool sleepOnReset;
    // when adding more values, keep the hash at the end!
    uint8_t hash[16];
} configValues;

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

bool checkConfigHash() {
    MD5Builder builder;
    uint8_t hash[16];
    bool ret;
    int len = offsetof(configValues_s, hash);
    builder.begin();
    builder.add((uint8_t *)&configValues, len);
    builder.calculate();
    builder.getBytes(hash);
    ret = (memcmp(hash, configValues.hash, 16) == 0);
    memcpy(configValues.hash, hash, 16);
    return ret;
}

void writeConfigValues() {
    Serial.println("Writing EEPROM");
    checkConfigHash(); // fill in the proper hash
    EEPROM.put(0, configValues);
    EEPROM.commit();
}

void readConfigValues() {
    EEPROM.get(0, configValues);
    if (!checkConfigHash()) {
        Serial.println("EEPROM data corrupt; initializing");
        // write defaults
        configValues.historyInterval = BME_DEFAULT_HISTORY_INTERVAL;
        configValues.sleepOnReset = false;
        writeConfigValues();
    }
}

String getEnvironmentData() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["time"] = sampleTime;
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
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["time"] = bmeHistory[i].time;
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
  server.sendContent("]}");
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
  StaticJsonBuffer<256> jsonBuffer;
  if (server.method() == HTTP_PUT) {
    String postData = server.arg("plain");
    if (postData != NULL) {
        bool needWrite = false;
        JsonObject& root = jsonBuffer.parseObject(postData, 1);
        if (root.is<unsigned int>("historyInterval")) {
            unsigned int interval = root.get<unsigned int>("historyInterval");
            if (configValues.historyInterval - measurementCount > interval) {
                // remaining time to next record is greater than the new
                // interval; shorten to new interval
                measurementCount = 0;
            }
            if (configValues.historyInterval != interval) {
                configValues.historyInterval = interval;
                needWrite = true;
            }
        }
        if (root.is<bool>("sleepOnReset")) {
            bool sleepOnReset = root.get<bool>("sleepOnReset");
            if (configValues.sleepOnReset != sleepOnReset) {
                configValues.sleepOnReset = sleepOnReset;
                needWrite = true;
            }
        }
        if (needWrite) {
            writeConfigValues();
        }
    }
  }
  jsonBuffer.clear();
  JsonObject& root = jsonBuffer.createObject();
  root["historyInterval"] = configValues.historyInterval;
  root["sleepOnReset"] = configValues.sleepOnReset;
  sendBuffer = "";
  root.printTo(sendBuffer);
  server.send(200, MIME_JSON, sendBuffer);
}

void setup() {
  bool wakeFromDeepSleep = (ESP.getResetInfoPtr()->reason == REASON_DEEP_SLEEP_AWAKE);

  sendBuffer.reserve(SEND_BUFFER_LEN);
  EEPROM.begin(sizeof(struct configValues_s));

  // initialize serial port
  Serial.begin(115200);
  Serial.println("\nReboot");

  readConfigValues();

  if (configValues.sleepOnReset && !wakeFromDeepSleep) {
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

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // wait for time to be initialized
  while (time(NULL) < 100000) {
    delay(200);
    // blink fast to indicate we're waiting for SNTP time
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

void loop() {
  // remember the last time the LED was toggled
  static unsigned long lastLedToggle = 0;
  // remember the last time a measurement was made
  static unsigned long lastMeasure = 0;

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
    lastMeasure = now;
    sampleTime = time(NULL);
    if (bmeActive && bme.performReading()) {
      Serial.println(getEnvironmentData());
    } else {
      Serial.println("Failed to perform reading :(");
    }
    if (++measurementCount >= configValues.historyInterval) {
      struct bmeSample_s* entry = &bmeHistory[bmeHistoryIndex];
      measurementCount = 0;
      entry->time = sampleTime;
      if (bmeActive) {
        entry->temperature = bme.temperature;
        entry->pressure = bme.pressure;
        entry->humidity = bme.humidity;
        entry->gas = bme.gas_resistance;
      } else {
        entry->temperature = INVALID_TEMPERATURE;
      }
      bmeHistoryIndex = (bmeHistoryIndex + 1) % BME_HISTORY_LEN;
      if (bmeHistoryCount < BME_HISTORY_LEN) {
        bmeHistoryCount += 1;
      }
    }
  }
  delay(100);
}
