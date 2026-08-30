/*
  ============================================================
  GLOBAL LIVE AUDIO RECORDER
  ESP8266-12E + INMP441
  ============================================================

  FEATURES
  ------------------------------------------------------------
  - Automatic Wi-Fi connection
  - Automatic Wi-Fi reconnection
  - Automatic MQTT connection
  - Unique recorder ID
  - Automatic recorder discovery
  - Microphone health detection
  - INMP441 I2S microphone
  - 16 kHz mono PCM16 audio
  - 20 ms audio packets
  - Live MQTT audio streaming
  - Automatic heartbeat
  - Website can show MIC ERROR
  - No local server required

  INMP441 WIRING
  ------------------------------------------------------------
  INMP441 VDD  -> ESP8266 3.3V
  INMP441 GND  -> ESP8266 GND
  INMP441 SCK  -> ESP8266 D7 / GPIO13
  INMP441 WS   -> ESP8266 D5 / GPIO14
  INMP441 SD   -> ESP8266 D6 / GPIO12
  INMP441 L/R  -> ESP8266 GND

  IMPORTANT:
  VDD MUST NOT be connected to 5V.

  ============================================================
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <I2S.h>


// ============================================================
// 1. WIFI SETTINGS
// ============================================================

const char* WIFI_SSID =
  "YOUR_HOTSPOT_NAME";

const char* WIFI_PASSWORD =
  "YOUR_HOTSPOT_PASSWORD";


// ============================================================
// 2. MQTT SETTINGS
// ============================================================

const char* MQTT_HOST =
  "broker.emqx.io";

const uint16_t MQTT_PORT =
  1883;


// ============================================================
// 3. RECORDER SETTINGS
// ============================================================

const char* DEVICE_NAME =
  "Recorder 1";


// ============================================================
// 4. AUDIO SETTINGS
// ============================================================

#define SAMPLE_RATE 16000

// 320 samples at 16 kHz = 20 milliseconds
#define AUDIO_SAMPLES 320

int16_t audioBuffer[AUDIO_SAMPLES];


// ============================================================
// 5. MQTT OBJECT
// ============================================================

WiFiClient wifiClient;

PubSubClient mqtt(
  wifiClient
);


// ============================================================
// 6. TOPICS
// ============================================================

String deviceId;

String baseTopic;

String discoveryTopic;

String audioTopic;


// ============================================================
// 7. STATUS
// ============================================================

bool microphoneOK = false;

bool i2sStarted = false;

unsigned long lastHeartbeat = 0;

unsigned long lastMicrophoneCheck = 0;

unsigned long lastWiFiAttempt = 0;

unsigned long lastMQTTAttempt = 0;


// ============================================================
// 8. TIMING
// ============================================================

const unsigned long HEARTBEAT_INTERVAL =
  5000;

const unsigned long MICROPHONE_CHECK_INTERVAL =
  3000;

const unsigned long WIFI_RETRY_INTERVAL =
  5000;

const unsigned long MQTT_RETRY_INTERVAL =
  5000;


// ============================================================
// 9. CONNECT WIFI
// ============================================================

void connectWiFi() {

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    return;
  }


  if (
    millis() - lastWiFiAttempt <
    WIFI_RETRY_INTERVAL
  ) {

    return;
  }


  lastWiFiAttempt =
    millis();


  Serial.println();
  Serial.println(
    "Connecting to Wi-Fi..."
  );


  WiFi.mode(
    WIFI_STA
  );


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  unsigned long start =
    millis();


  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  ) {

    delay(250);

    Serial.print(".");
  }


  Serial.println();


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "Wi-Fi connected!"
    );


    Serial.print(
      "IP address: "
    );


    Serial.println(
      WiFi.localIP()
    );

  }

  else {

    Serial.println(
      "Wi-Fi connection failed."
    );

  }
}


// ============================================================
// 10. MQTT CONNECT
// ============================================================

void connectMQTT() {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return;
  }


  if (
    mqtt.connected()
  ) {

    return;
  }


  if (
    millis() - lastMQTTAttempt <
    MQTT_RETRY_INTERVAL
  ) {

    return;
  }


  lastMQTTAttempt =
    millis();


  Serial.println(
    "Connecting to MQTT..."
  );


  String clientId =
    deviceId +
    "-" +
    String(
      ESP.getChipId(),
      HEX
    );


  if (
    mqtt.connect(
      clientId.c_str()
    )
  ) {

    Serial.println(
      "MQTT connected!"
    );


    publishStatus();

  }

  else {

    Serial.print(
      "MQTT connection failed. State: "
    );


    Serial.println(
      mqtt.state()
    );

  }
}


// ============================================================
// 11. START I2S
// ============================================================

void startI2S() {

  if (
    i2sStarted
  ) {

    return;
  }


  Serial.println(
    "Starting I2S microphone..."
  );


  bool result =
    i2s_rxtx_begin(
      true,
      false
    );


  if (!result) {

    Serial.println(
      "ERROR: Could not start I2S."
    );

    i2sStarted =
      false;

    microphoneOK =
      false;

    return;
  }


  i2s_set_rate(
    SAMPLE_RATE
  );


  i2sStarted =
    true;


  Serial.println(
    "I2S started."
  );
}


// ============================================================
// 12. READ ONE AUDIO BLOCK
// ============================================================

bool readAudioBlock() {

  if (
    !i2sStarted
  ) {

    return false;
  }


  for (
    int i = 0;
    i < AUDIO_SAMPLES;
    i++
  ) {

    int16_t left =
      0;

    int16_t right =
      0;


    bool result =
      i2s_read_sample(
        &left,
        &right,
        true
      );


    if (!result) {

      return false;
    }


    /*
      L/R is connected to GND,
      therefore the INMP441 is
      configured for the LEFT channel.

      We use LEFT here.
    */

    audioBuffer[i] =
      left;
  }


  return true;
}


// ============================================================
// 13. MICROPHONE DETECTION
// ============================================================

bool testMicrophone() {

  if (
    !i2sStarted
  ) {

    return false;
  }


  long minimum =
    32767;

  long maximum =
    -32768;


  bool received =
    false;


  /*
    Take 640 samples for the
    microphone diagnostic.
  */

  for (
    int i = 0;
    i < 640;
    i++
  ) {

    int16_t left =
      0;

    int16_t right =
      0;


    bool result =
      i2s_read_sample(
        &left,
        &right,
        true
      );


    if (!result) {

      continue;
    }


    received =
      true;


    long value =
      (long)left;


    if (
      value <
      minimum
    ) {

      minimum =
        value;
    }


    if (
      value >
      maximum
    ) {

      maximum =
        value;
    }
  }


  if (!received) {

    return false;
  }


  long variation =
    maximum -
    minimum;


  /*
    Completely dead/floating I2S
    data commonly appears as a
    constant or nearly constant
    value.

    We intentionally use a
    relatively small threshold
    because microphone noise can
    exist even in silence.
  */

  if (
    variation <= 8
  ) {

    return false;
  }


  return true;
}


// ============================================================
// 14. UPDATE MICROPHONE STATUS
// ============================================================

void updateMicrophoneStatus() {

  if (
    millis() - lastMicrophoneCheck <
    MICROPHONE_CHECK_INTERVAL
  ) {

    return;
  }


  lastMicrophoneCheck =
    millis();


  bool detected =
    testMicrophone();


  if (detected) {

    if (!microphoneOK) {

      Serial.println();
      Serial.println(
        "================================"
      );

      Serial.println(
        "MICROPHONE STATUS: OK"
      );

      Serial.println(
        "INMP441 signal detected."
      );

      Serial.println(
        "================================"
      );
    }


    microphoneOK =
      true;

  }

  else {

    if (microphoneOK) {

      Serial.println();
      Serial.println(
        "================================"
      );

      Serial.println(
        "MICROPHONE ERROR"
      );

      Serial.println(
        "INMP441 signal lost."
      );

      Serial.println(
        "================================"
      );
    }


    microphoneOK =
      false;
  }


  publishStatus();
}


// ============================================================
// 15. PUBLISH DEVICE STATUS
// ============================================================

void publishStatus() {

  if (
    !mqtt.connected()
  ) {

    return;
  }


  StaticJsonDocument<384> doc;


  doc["peerId"] =
    deviceId;


  doc["name"] =
    DEVICE_NAME;


  if (
    microphoneOK
  ) {

    doc["status"] =
      "LIVE";

    doc["microphone"] =
      "OK";

  }

  else {

    doc["status"] =
      "MIC_ERROR";

    doc["microphone"] =
      "NOT_DETECTED";
  }


  doc["sampleRate"] =
    SAMPLE_RATE;


  doc["channels"] =
    1;


  doc["format"] =
    "PCM16";


  doc["audioTopic"] =
    audioTopic;


  doc["ip"] =
    WiFi.localIP().toString();


  char payload[384];


  serializeJson(
    doc,
    payload
  );


  mqtt.publish(
    discoveryTopic.c_str(),
    payload,
    true
  );
}


// ============================================================
// 16. SEND AUDIO
// ============================================================

void sendAudio() {

  if (
    !microphoneOK
  ) {

    return;
  }


  if (
    !mqtt.connected()
  ) {

    return;
  }


  /*
    320 samples
    × 2 bytes
    =
    640 bytes.
  */

  bool sent =
    mqtt.publish(
      audioTopic.c_str(),

      (const uint8_t*)
        audioBuffer,

      AUDIO_SAMPLES * 2,

      false
    );


  if (!sent) {

    /*
      Do not spam Serial constantly.
    */

    static unsigned long lastError =
      0;


    if (
      millis() -
      lastError >
      3000
    ) {

      lastError =
        millis();


      Serial.println(
        "WARNING: Audio packet failed."
      );
    }
  }
}


// ============================================================
// 17. SETUP
// ============================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(1000);


  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "GLOBAL LIVE AUDIO RECORDER"
  );

  Serial.println(
    "ESP8266-12E + INMP441"
  );

  Serial.println(
    "=========================================="
  );


  /*
    Generate unique ID from
    ESP8266 chip ID.
  */

  deviceId =
    "recorder-" +
    String(
      ESP.getChipId(),
      HEX
    );


  Serial.print(
    "Device ID: "
  );


  Serial.println(
    deviceId
  );


  /*
    Create MQTT topics.
  */

  baseTopic =
    "global-audio/" +
    deviceId;


  discoveryTopic =
    "global-audio/discovery";


  audioTopic =
    baseTopic +
    "/audio";


  Serial.print(
    "Audio topic: "
  );


  Serial.println(
    audioTopic
  );


  /*
    MQTT configuration.
  */

  mqtt.setServer(
    MQTT_HOST,
    MQTT_PORT
  );


  /*
    2048 gives enough room for
    topic + 640-byte audio packet.
  */

  mqtt.setBufferSize(
    2048
  );


  /*
    Wi-Fi.
  */

  connectWiFi();


  /*
    I2S.
  */

  startI2S();


  /*
    Initial microphone test.
  */

  Serial.println(
    "Checking INMP441..."
  );


  microphoneOK =
    testMicrophone();


  if (
    microphoneOK
  ) {

    Serial.println(
      "MICROPHONE: OK"
    );

  }

  else {

    Serial.println();
    Serial.println(
      "********************************"
    );

    Serial.println(
      "MICROPHONE ERROR"
    );

    Serial.println(
      "INMP441 not detected."
    );

    Serial.println(
      "Check the wiring."
    );

    Serial.println(
      "********************************"
    );
  }


  Serial.println();
  Serial.println(
    "SYSTEM INITIALIZED."
  );
}


// ============================================================
// 18. MAIN LOOP
// ============================================================

void loop() {

  /*
    Wi-Fi.
  */

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    connectWiFi();
  }


  /*
    MQTT.
  */

  if (
    !mqtt.connected()
  ) {

    connectMQTT();
  }


  /*
    MQTT processing.
  */

  mqtt.loop();


  /*
    Microphone health check.
  */

  updateMicrophoneStatus();


  /*
    Heartbeat.
  */

  if (
    millis() -
    lastHeartbeat >=
    HEARTBEAT_INTERVAL
  ) {

    lastHeartbeat =
      millis();


    publishStatus();
  }


  /*
    Audio.

    IMPORTANT:
    Only send audio when the
    microphone is considered OK.
  */

  if (
    microphoneOK
  ) {

    if (
      readAudioBlock()
    ) {

      sendAudio();

    }

  }


  /*
    Give ESP8266 background
    networking time.
  */

  yield();
}
