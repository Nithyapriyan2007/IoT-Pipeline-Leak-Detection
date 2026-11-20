/*
  ============================================================
       IoT PIPELINE LEAK DETECTION SYSTEM
       ESP32 + 2 FLOW SENSORS + RELAY + BUZZER
       + OLED + HC-SR04 + TELEGRAM + WEB DASHBOARD

  FLOW SENSOR 1 -> GPIO 25 -> WATER IN
  FLOW SENSOR 2 -> GPIO 26 -> WATER OUT

  RELAY         -> GPIO 27
  RELAY ACTIVE LOW:
      LOW  = PUMP ON
      HIGH = PUMP OFF

  BUZZER        -> GPIO 14
  LED           -> GPIO 2

  HC-SR04:
      TRIG -> GPIO 32
      ECHO -> GPIO 33
      IMPORTANT: ECHO MUST GO THROUGH A VOLTAGE DIVIDER
                 BEFORE CONNECTING TO ESP32 GPIO 33

  OLED SSD1306:
      SDA -> GPIO 21
      SCL -> GPIO 22
      VCC -> 3.3V
      GND -> GND
      ADDRESS -> 0x3C

  LEAK DETECTION:
      Difference >= 0.70 L/min
      for 2 consecutive readings
      = LEAK

  FLOW SMOOTHING:
      5-reading moving average

  PULSES PER LITRE:
      450.0  <-- DO NOT CHANGE
  ============================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


// ============================================================
// WIFI SETTINGS
// ============================================================

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


// ============================================================
// TELEGRAM SETTINGS
// ============================================================

const char* BOT_TOKEN = "YOUR_NEW_TELEGRAM_BOT_TOKEN";
const char* CHAT_ID   = "YOUR_CHAT_ID";


// ============================================================
// ESP32 ACCESS POINT
// ============================================================

const char* AP_SSID = "Pipeline-ESP32";
const char* AP_PASSWORD = "12345678";


// ============================================================
// PIN DEFINITIONS
// ============================================================

#define FLOW_SENSOR_1_PIN 25
#define FLOW_SENSOR_2_PIN 26

#define RELAY_PIN 27
#define BUZZER_PIN 14
#define LED_PIN 2

#define ULTRASONIC_TRIG_PIN 32
#define ULTRASONIC_ECHO_PIN 33


// ============================================================
// OLED
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

bool oledOK = false;


// ============================================================
// FLOW CALIBRATION
// ============================================================

const float PULSES_PER_LITRE = 450.0;


// ============================================================
// LEAK SETTINGS
// ============================================================

// Leak when difference reaches 0.70 L/min
const float LEAK_THRESHOLD = 0.70;

// Minimum inlet flow required before checking for leak
const float MIN_FLOW_FOR_LEAK = 1.00;

// TWO consecutive readings
const int REQUIRED_LEAK_READINGS = 2;


// ============================================================
// TIMING
// ============================================================

const unsigned long FLOW_INTERVAL = 1000;

const unsigned long STARTUP_GRACE_PERIOD = 10000;

const unsigned long MIN_PULSE_INTERVAL_US = 5000;

const unsigned long WIFI_CHECK_INTERVAL = 10000;

const unsigned long ULTRASONIC_INTERVAL = 500;

const unsigned long OLED_UPDATE_INTERVAL = 500;


// ============================================================
// FLOW SMOOTHING
// ============================================================

const int FLOW_AVERAGE_SAMPLES = 5;

float flowInBuffer[FLOW_AVERAGE_SAMPLES];
float flowOutBuffer[FLOW_AVERAGE_SAMPLES];

int flowBufferIndex = 0;
int flowBufferCount = 0;


// ============================================================
// FLOW VARIABLES
// ============================================================

volatile unsigned long pulsesFlow1 = 0;
volatile unsigned long pulsesFlow2 = 0;

volatile unsigned long lastPulseTime1 = 0;
volatile unsigned long lastPulseTime2 = 0;


// Raw flow values
float rawFlowIn = 0.0;
float rawFlowOut = 0.0;


// Smoothed flow values
float flowIn = 0.0;
float flowOut = 0.0;
float flowDifference = 0.0;


// ============================================================
// ULTRASONIC VARIABLES
// ============================================================

float distanceCM = -1.0;

float leakDistanceCM = -1.0;

bool distanceValid = false;

unsigned long lastUltrasonicTime = 0;


// ============================================================
// SYSTEM VARIABLES
// ============================================================

bool pumpState = false;

bool leakDetected = false;

bool telegramConnected = false;

bool leakAlertSent = false;

int leakCounter = 0;


// This is important.
// It makes sure one flow reading is counted only once.
bool newFlowReading = false;


// ============================================================
// TIMERS
// ============================================================

unsigned long lastFlowTime = 0;

unsigned long pumpStartedTime = 0;

unsigned long lastWiFiCheck = 0;

unsigned long lastOLEDTime = 0;


// ============================================================
// WEB SERVER
// ============================================================

WiFiServer server(80);


// ============================================================
// INTERRUPT SERVICE ROUTINES
// ============================================================

void IRAM_ATTR flowSensor1ISR()
{
  unsigned long now = micros();

  if ((now - lastPulseTime1) >= MIN_PULSE_INTERVAL_US)
  {
    pulsesFlow1++;
    lastPulseTime1 = now;
  }
}


void IRAM_ATTR flowSensor2ISR()
{
  unsigned long now = micros();

  if ((now - lastPulseTime2) >= MIN_PULSE_INTERVAL_US)
  {
    pulsesFlow2++;
    lastPulseTime2 = now;
  }
}


// ============================================================
// RESET FLOW FILTER
// ============================================================

void resetFlowFilter()
{
  flowBufferIndex = 0;
  flowBufferCount = 0;

  rawFlowIn = 0.0;
  rawFlowOut = 0.0;

  flowIn = 0.0;
  flowOut = 0.0;
  flowDifference = 0.0;

  for (int i = 0; i < FLOW_AVERAGE_SAMPLES; i++)
  {
    flowInBuffer[i] = 0.0;
    flowOutBuffer[i] = 0.0;
  }
}


// ============================================================
// UPDATE FLOW AVERAGE
// ============================================================

void updateFlowAverage(float inFlow, float outFlow)
{
  flowInBuffer[flowBufferIndex] = inFlow;
  flowOutBuffer[flowBufferIndex] = outFlow;

  flowBufferIndex++;

  if (flowBufferIndex >= FLOW_AVERAGE_SAMPLES)
  {
    flowBufferIndex = 0;
  }

  if (flowBufferCount < FLOW_AVERAGE_SAMPLES)
  {
    flowBufferCount++;
  }


  float sumIn = 0.0;
  float sumOut = 0.0;


  for (int i = 0; i < flowBufferCount; i++)
  {
    sumIn += flowInBuffer[i];
    sumOut += flowOutBuffer[i];
  }


  flowIn = sumIn / flowBufferCount;

  flowOut = sumOut / flowBufferCount;


  // Small measurement noise
  if (flowIn < 0.05)
  {
    flowIn = 0.0;
  }

  if (flowOut < 0.05)
  {
    flowOut = 0.0;
  }


  // Only positive difference is considered
  flowDifference = flowIn - flowOut;

  if (flowDifference < 0.0)
  {
    flowDifference = 0.0;
  }
}


// ============================================================
// CALCULATE FLOW
// ============================================================

void calculateFlow()
{
  if (millis() - lastFlowTime < FLOW_INTERVAL)
  {
    return;
  }

  lastFlowTime = millis();


  // Safely copy pulse counts
  noInterrupts();

  unsigned long pulses1 = pulsesFlow1;
  unsigned long pulses2 = pulsesFlow2;

  pulsesFlow1 = 0;
  pulsesFlow2 = 0;

  interrupts();


  // Convert pulses to L/min
  rawFlowIn =
    ((float)pulses1 / PULSES_PER_LITRE) * 60.0;

  rawFlowOut =
    ((float)pulses2 / PULSES_PER_LITRE) * 60.0;


  // Apply smoothing
  updateFlowAverage(rawFlowIn, rawFlowOut);


  // Tell leak detection that a NEW reading is available
  newFlowReading = true;


  // Serial monitor
  Serial.println();
  Serial.println("========== FLOW ==========");
  Serial.print("Raw IN  : ");
  Serial.print(rawFlowIn, 2);
  Serial.println(" L/min");

  Serial.print("Raw OUT : ");
  Serial.print(rawFlowOut, 2);
  Serial.println(" L/min");

  Serial.print("Smooth IN  : ");
  Serial.print(flowIn, 2);
  Serial.println(" L/min");

  Serial.print("Smooth OUT : ");
  Serial.print(flowOut, 2);
  Serial.println(" L/min");

  Serial.print("Difference : ");
  Serial.print(flowDifference, 2);
  Serial.println(" L/min");

  Serial.println("==========================");
}


// ============================================================
// PUMP ON
// ============================================================

void pumpON()
{
  digitalWrite(RELAY_PIN, LOW);

  pumpState = true;

  pumpStartedTime = millis();

  leakDetected = false;

  leakAlertSent = false;

  leakCounter = 0;

  leakDistanceCM = -1.0;

  newFlowReading = false;

  resetFlowFilter();

  Serial.println("PUMP: ON");
}


// ============================================================
// PUMP OFF
// ============================================================

void pumpOFF()
{
  digitalWrite(RELAY_PIN, HIGH);

  pumpState = false;

  Serial.println("PUMP: OFF");
}


// ============================================================
// ULTRASONIC SINGLE READING
// ============================================================

float readUltrasonicOnce()
{
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  delayMicroseconds(2);

  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);

  delayMicroseconds(10);

  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);


  unsigned long duration =
    pulseIn(
      ULTRASONIC_ECHO_PIN,
      HIGH,
      25000
    );


  if (duration == 0)
  {
    return -1.0;
  }


  float distance =
    (duration * 0.0343) / 2.0;


  if (distance < 2.0 || distance > 400.0)
  {
    return -1.0;
  }


  return distance;
}


// ============================================================
// MEDIAN OF 3 DISTANCE VALUES
// ============================================================

float median3(float a, float b, float c)
{
  if (a > b)
  {
    float temp = a;
    a = b;
    b = temp;
  }

  if (b > c)
  {
    float temp = b;
    b = c;
    c = temp;
  }

  if (a > b)
  {
    float temp = a;
    a = b;
    b = temp;
  }

  return b;
}


// ============================================================
// UPDATE ULTRASONIC
// ============================================================

void updateUltrasonic()
{
  if (millis() - lastUltrasonicTime < ULTRASONIC_INTERVAL)
  {
    return;
  }

  lastUltrasonicTime = millis();


  float d1 = readUltrasonicOnce();

  delay(5);

  float d2 = readUltrasonicOnce();

  delay(5);

  float d3 = readUltrasonicOnce();


  int validCount = 0;

  if (d1 > 0) validCount++;
  if (d2 > 0) validCount++;
  if (d3 > 0) validCount++;


  if (validCount == 3)
  {
    distanceCM = median3(d1, d2, d3);

    distanceValid = true;
  }
  else if (validCount == 2)
  {
    if (d1 <= 0)
    {
      distanceCM = (d2 + d3) / 2.0;
    }
    else if (d2 <= 0)
    {
      distanceCM = (d1 + d3) / 2.0;
    }
    else
    {
      distanceCM = (d1 + d2) / 2.0;
    }

    distanceValid = true;
  }
  else if (validCount == 1)
  {
    if (d1 > 0)
      distanceCM = d1;
    else if (d2 > 0)
      distanceCM = d2;
    else
      distanceCM = d3;

    distanceValid = true;
  }

  // IMPORTANT:
  // If no valid reading is received,
  // keep the previous distance instead of displaying --.


  if (distanceValid)
  {
    Serial.print("HC-SR04 Distance: ");
    Serial.print(distanceCM, 1);
    Serial.println(" cm");
  }
}


// ============================================================
// TELEGRAM URL ENCODE
// ============================================================

String urlEncode(String text)
{
  String encoded = "";

  char c;

  char code0;
  char code1;

  for (int i = 0; i < text.length(); i++)
  {
    c = text.charAt(i);

    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' ||
      c == '_' ||
      c == '.' ||
      c == '~'
    )
    {
      encoded += c;
    }
    else if (c == '\n')
    {
      encoded += "%0A";
    }
    else
    {
      code1 = (c & 0xF) + '0';

      if (code1 > '9')
        code1 += 7;

      code0 = ((c >> 4) & 0xF) + '0';

      if (code0 > '9')
        code0 += 7;

      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }

  return encoded;
}


// ============================================================
// SEND TELEGRAM MESSAGE
// ============================================================

bool sendTelegramMessage(String message)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Telegram: WiFi not connected");

    telegramConnected = false;

    return false;
  }


  WiFiClientSecure client;

  client.setInsecure();


  HTTPClient https;


  String url =
    "https://api.telegram.org/bot" +
    String(BOT_TOKEN) +
    "/sendMessage?chat_id=" +
    String(CHAT_ID) +
    "&text=" +
    urlEncode(message);


  Serial.println("Sending Telegram message...");


  if (!https.begin(client, url))
  {
    Serial.println("Telegram HTTPS begin failed");

    telegramConnected = false;

    return false;
  }


  int httpCode = https.GET();


  if (httpCode > 0)
  {
    Serial.print("Telegram HTTP Code: ");
    Serial.println(httpCode);

    if (httpCode == 200)
    {
      telegramConnected = true;

      https.end();

      return true;
    }
  }
  else
  {
    Serial.print("Telegram Error: ");
    Serial.println(httpCode);
  }


  telegramConnected = false;

  https.end();

  return false;
}


// ============================================================
// LEAK DETECTION
// ============================================================

void checkLeak()
{
  // Only check when a NEW 1-second flow reading arrives
  if (!newFlowReading)
  {
    return;
  }

  // Consume this reading
  newFlowReading = false;


  // Pump must be running
  if (!pumpState)
  {
    leakCounter = 0;

    return;
  }


  // Startup grace period
  if (millis() - pumpStartedTime < STARTUP_GRACE_PERIOD)
  {
    leakCounter = 0;

    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    return;
  }


  // Not enough water flow
  if (flowIn < MIN_FLOW_FOR_LEAK)
  {
    leakCounter = 0;

    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    return;
  }


  // ========================================================
  // LEAK CONDITION
  // Difference must REACH 0.70
  // ========================================================

  if (flowDifference >= LEAK_THRESHOLD)
  {
    leakCounter++;


    Serial.print("Possible leak reading: ");
    Serial.print(leakCounter);
    Serial.print("/");
    Serial.println(REQUIRED_LEAK_READINGS);


    // TWO consecutive readings
    if (leakCounter >= REQUIRED_LEAK_READINGS)
    {
      leakDetected = true;


      // Save distance at leak confirmation
      if (distanceValid)
      {
        leakDistanceCM = distanceCM;
      }
      else
      {
        leakDistanceCM = -1.0;
      }


      // Stop pump
      pumpOFF();


      // Alarm ON
      digitalWrite(BUZZER_PIN, HIGH);
      digitalWrite(LED_PIN, HIGH);


      Serial.println();
      Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!");
      Serial.println("      LEAK DETECTED");
      Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!");


      Serial.print("IN: ");
      Serial.print(flowIn, 2);
      Serial.println(" L/min");

      Serial.print("OUT: ");
      Serial.print(flowOut, 2);
      Serial.println(" L/min");

      Serial.print("Difference: ");
      Serial.print(flowDifference, 2);
      Serial.println(" L/min");

      Serial.print("Leak Distance: ");

      if (leakDistanceCM > 0)
      {
        Serial.print(leakDistanceCM, 1);
        Serial.println(" cm");
      }
      else
      {
        Serial.println("N/A");
      }


      // Telegram only once
      if (!leakAlertSent)
      {
        String message = "🚨 LEAK DETECTED!\n\n";

        message += "IN Flow: ";
        message += String(flowIn, 2);
        message += " L/min\n";

        message += "OUT Flow: ";
        message += String(flowOut, 2);
        message += " L/min\n";

        message += "Difference: ";
        message += String(flowDifference, 2);
        message += " L/min\n";

        if (leakDistanceCM > 0)
        {
          message += "HC-SR04 Distance: ";
          message += String(leakDistanceCM, 1);
          message += " cm\n";
        }
        else
        {
          message += "HC-SR04 Distance: N/A\n";
        }

        message += "\nPump: OFF";

        if (sendTelegramMessage(message))
        {
          leakAlertSent = true;
        }
      }
    }
  }
  else
  {
    // Difference returned below threshold
    leakCounter = 0;

    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
  }
}


// ============================================================
// OLED UPDATE
// ============================================================

void updateOLED()
{
  // Don't update continuously
  if (millis() - lastOLEDTime < OLED_UPDATE_INTERVAL)
  {
    return;
  }

  lastOLEDTime = millis();


  if (!oledOK)
  {
    return;
  }


  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);


  // TITLE
  display.setCursor(0, 0);

  display.println("PIPELINE MONITOR");


  // FLOW IN
  display.setCursor(0, 11);

  display.print("IN : ");

  display.print(flowIn, 2);

  display.println(" L/m");


  // FLOW OUT
  display.setCursor(0, 21);

  display.print("OUT: ");

  display.print(flowOut, 2);

  display.println(" L/m");


  // DIFFERENCE
  display.setCursor(0, 31);

  display.print("DIF: ");

  display.print(flowDifference, 2);

  display.println(" L/m");


  // DISTANCE
  display.setCursor(0, 41);

  display.print("DIST: ");

  if (distanceValid)
  {
    display.print(distanceCM, 1);
    display.println(" cm");
  }
  else
  {
    display.println("--");
  }


  // STATUS
  display.setCursor(0, 53);


  if (leakDetected)
  {
    display.print("LEAK!");

    if (leakDistanceCM > 0)
    {
      display.print(" @");
      display.print(leakDistanceCM, 0);
      display.print("cm");
    }
  }
  else if (millis() - pumpStartedTime < STARTUP_GRACE_PERIOD)
  {
    display.print("P:ON START");
  }
  else
  {
    display.print("P:");

    if (pumpState)
      display.print("ON ");
    else
      display.print("OFF ");

    display.print("NORMAL");
  }


  display.display();
}


// ============================================================
// RESET SYSTEM
// ============================================================

void resetSystem()
{
  Serial.println("SYSTEM RESET");


  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);


  leakDetected = false;

  leakAlertSent = false;

  leakCounter = 0;

  leakDistanceCM = -1.0;

  distanceCM = -1.0;

  distanceValid = false;


  noInterrupts();

  pulsesFlow1 = 0;
  pulsesFlow2 = 0;

  interrupts();


  resetFlowFilter();


  pumpON();


  sendTelegramMessage(
    "🔄 Pipeline system reset.\nPump: ON"
  );
}


// ============================================================
// WIFI CHECK
// ============================================================

void checkWiFi()
{
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL)
  {
    return;
  }

  lastWiFiCheck = millis();


  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi disconnected. Reconnecting...");

    WiFi.disconnect();

    WiFi.begin(
      WIFI_SSID,
      WIFI_PASSWORD
    );
  }
}


// ============================================================
// WEB PAGE
// ============================================================

String webPage()
{
  String page = R"rawliteral(
<!DOCTYPE html>
<html>

<head>

<meta name="viewport"
content="width=device-width, initial-scale=1">

<title>Pipeline Leak Detection</title>

<style>

body{
  font-family:Arial,sans-serif;
  background:#f2f5f8;
  margin:0;
  padding:20px;
  color:#222;
}

.container{
  max-width:900px;
  margin:auto;
}

h1{
  text-align:center;
  margin-bottom:20px;
}

.grid{
  display:grid;
  grid-template-columns:
  repeat(auto-fit,minmax(200px,1fr));
  gap:15px;
}

.card{
  background:white;
  padding:20px;
  border-radius:15px;
  box-shadow:
  0 4px 15px rgba(0,0,0,.10);
  text-align:center;
}

.title{
  font-size:15px;
  color:#666;
}

.value{
  font-size:28px;
  font-weight:bold;
  margin-top:10px;
}

.normal{
  color:green;
}

.leak{
  color:red;
}

.info{
  margin-top:20px;
  background:white;
  padding:20px;
  border-radius:15px;
}

button{
  border:none;
  padding:14px 25px;
  border-radius:10px;
  background:#222;
  color:white;
  font-size:16px;
}

</style>

</head>

<body>

<div class="container">

<h1>🚰 Pipeline Leak Detection</h1>

<div class="grid">

<div class="card">
<div class="title">FLOW IN</div>
<div class="value" id="in">--</div>
</div>

<div class="card">
<div class="title">FLOW OUT</div>
<div class="value" id="out">--</div>
</div>

<div class="card">
<div class="title">DIFFERENCE</div>
<div class="value" id="diff">--</div>
</div>

<div class="card">
<div class="title">HC-SR04 DISTANCE</div>
<div class="value" id="distance">--</div>
</div>

<div class="card">
<div class="title">LEAK DISTANCE</div>
<div class="value" id="leakDistance">--</div>
</div>

<div class="card">
<div class="title">PUMP</div>
<div class="value" id="pump">--</div>
</div>

<div class="card">
<div class="title">LEAK STATUS</div>
<div class="value" id="leak">--</div>
</div>

<div class="card">
<div class="title">WiFi</div>
<div class="value" id="wifi">--</div>
</div>

<div class="card">
<div class="title">Telegram</div>
<div class="value" id="telegram">--</div>
</div>

</div>


<div class="info">

<b>Leak threshold:</b>
0.70 L/min

<br><br>

<b>Confirmation:</b>
2 consecutive readings

<br><br>

<button onclick="resetSystem()">
RESET SYSTEM
</button>

</div>

</div>


<script>

function updateData(){

fetch('/data')

.then(response => response.json())

.then(data => {

document.getElementById('in').innerHTML =
data.flowIn.toFixed(2) + " L/min";

document.getElementById('out').innerHTML =
data.flowOut.toFixed(2) + " L/min";

document.getElementById('diff').innerHTML =
data.difference.toFixed(2) + " L/min";


if(data.distance >= 0)
{
document.getElementById('distance').innerHTML =
data.distance.toFixed(1) + " cm";
}
else
{
document.getElementById('distance').innerHTML =
"--";
}


if(data.leakDistance >= 0)
{
document.getElementById('leakDistance').innerHTML =
data.leakDistance.toFixed(1) + " cm";
}
else
{
document.getElementById('leakDistance').innerHTML =
"--";
}


document.getElementById('pump').innerHTML =
data.pump ? "ON" : "OFF";


let leakElement =
document.getElementById('leak');

if(data.leak)
{
leakElement.innerHTML =
"🚨 LEAK DETECTED";

leakElement.className =
"value leak";
}
else
{
leakElement.innerHTML =
"✓ NORMAL";

leakElement.className =
"value normal";
}


document.getElementById('wifi').innerHTML =
data.wifi ? "CONNECTED" : "DISCONNECTED";


document.getElementById('telegram').innerHTML =
data.telegram ? "CONNECTED" : "NOT CONNECTED";

});

}


function resetSystem(){

fetch('/reset')
.then(() => {
setTimeout(updateData,500);
});

}


setInterval(updateData,1000);

updateData();

</script>

</body>

</html>
)rawliteral";


  return page;
}


// ============================================================
// HANDLE WEBSITE
// ============================================================

void handleWebsite()
{
  WiFiClient client = server.available();


  if (!client)
  {
    return;
  }


  unsigned long timeout = millis();


  while (
    client.connected() &&
    !client.available()
  )
  {
    if (millis() - timeout > 1000)
    {
      client.stop();

      return;
    }

    delay(1);
  }


  String request = client.readStringUntil('\r');

  client.flush();


  // ========================================================
  // MAIN PAGE
  // ========================================================

  if (
    request.indexOf("GET / ") >= 0 ||
    request.indexOf("GET /HTTP") >= 0
  )
  {
    String page = webPage();


    client.println(
      "HTTP/1.1 200 OK"
    );

    client.println(
      "Content-Type: text/html"
    );

    client.println(
      "Connection: close"
    );

    client.println();

    client.print(page);
  }


  // ========================================================
  // DATA
  // ========================================================

  else if (
    request.indexOf("GET /data") >= 0
  )
  {
    String json = "{";

    json += "\"flowIn\":";
    json += String(flowIn, 2);

    json += ",\"flowOut\":";
    json += String(flowOut, 2);

    json += ",\"difference\":";
    json += String(flowDifference, 2);

    json += ",\"distance\":";
    json += String(distanceCM, 1);

    json += ",\"leakDistance\":";
    json += String(leakDistanceCM, 1);

    json += ",\"pump\":";
    json += pumpState ? "true" : "false";

    json += ",\"leak\":";
    json += leakDetected ? "true" : "false";

    json += ",\"wifi\":";
    json +=
      WiFi.status() == WL_CONNECTED
      ? "true"
      : "false";

    json += ",\"telegram\":";
    json += telegramConnected
      ? "true"
      : "false";

    json += "}";


    client.println(
      "HTTP/1.1 200 OK"
    );

    client.println(
      "Content-Type: application/json"
    );

    client.println(
      "Cache-Control: no-cache"
    );

    client.println(
      "Connection: close"
    );

    client.println();

    client.print(json);
  }


  // ========================================================
  // RESET
  // ========================================================

  else if (
    request.indexOf("GET /reset") >= 0
  )
  {
    resetSystem();


    client.println(
      "HTTP/1.1 200 OK"
    );

    client.println(
      "Content-Type: text/plain"
    );

    client.println(
      "Connection: close"
    );

    client.println();

    client.println("System Reset");
  }


  else
  {
    client.println(
      "HTTP/1.1 404 Not Found"
    );

    client.println(
      "Connection: close"
    );

    client.println();

    client.println("404");
  }


  delay(1);

  client.stop();
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);


  Serial.println();
  Serial.println();
  Serial.println(
    "======================================"
  );

  Serial.println(
    "   IoT PIPELINE LEAK DETECTION"
  );

  Serial.println(
    "======================================"
  );


  // ========================================================
  // PIN SETUP
  // ========================================================

  pinMode(FLOW_SENSOR_1_PIN, INPUT_PULLUP);

  pinMode(FLOW_SENSOR_2_PIN, INPUT_PULLUP);


  pinMode(RELAY_PIN, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(LED_PIN, OUTPUT);


  pinMode(
    ULTRASONIC_TRIG_PIN,
    OUTPUT
  );

  pinMode(
    ULTRASONIC_ECHO_PIN,
    INPUT
  );


  // Start safe
  digitalWrite(RELAY_PIN, HIGH);

  digitalWrite(BUZZER_PIN, LOW);

  digitalWrite(LED_PIN, LOW);

  digitalWrite(
    ULTRASONIC_TRIG_PIN,
    LOW
  );


  // ========================================================
  // OLED SETUP
  // ========================================================

  Wire.begin(21, 22);

  // Conservative I2C speed for reliability
  Wire.setClock(100000);


  if (
    display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS
    )
  )
  {
    oledOK = true;

    display.clearDisplay();

    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);

    display.setCursor(0, 0);

    display.println(
      "PIPELINE MONITOR"
    );

    display.setCursor(0, 15);

    display.println(
      "Starting..."
    );

    display.display();

    Serial.println(
      "OLED: OK"
    );
  }
  else
  {
    oledOK = false;

    Serial.println(
      "OLED: NOT FOUND"
    );
  }


  // ========================================================
  // FLOW INTERRUPTS
  // ========================================================

  attachInterrupt(
    digitalPinToInterrupt(
      FLOW_SENSOR_1_PIN
    ),
    flowSensor1ISR,
    RISING
  );


  attachInterrupt(
    digitalPinToInterrupt(
      FLOW_SENSOR_2_PIN
    ),
    flowSensor2ISR,
    RISING
  );


  // ========================================================
  // FLOW FILTER
  // ========================================================

  resetFlowFilter();


  // ========================================================
  // WIFI
  // ========================================================

  WiFi.mode(WIFI_AP_STA);


  Serial.println();
  Serial.println(
    "Connecting to WiFi..."
  );


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  unsigned long wifiStart =
    millis();


  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - wifiStart < 15000
  )
  {
    delay(500);

    Serial.print(".");
  }


  Serial.println();


  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println(
      "WiFi connected"
    );

    Serial.print(
      "WiFi IP: "
    );

    Serial.println(
      WiFi.localIP()
    );
  }
  else
  {
    Serial.println(
      "WiFi connection failed"
    );
  }


  // ========================================================
  // ACCESS POINT
  // ========================================================

  WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );


  Serial.println();

  Serial.println(
    "ESP32 Access Point:"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    AP_SSID
  );

  Serial.print(
    "Password: "
  );

  Serial.println(
    AP_PASSWORD
  );

  Serial.print(
    "AP IP: "
  );

  Serial.println(
    WiFi.softAPIP()
  );


  // ========================================================
  // WEB SERVER
  // ========================================================

  server.begin();

  Serial.println(
    "Web server started"
  );


  // ========================================================
  // START PUMP
  // ========================================================

  pumpON();


  // Set timers after pump starts
  lastFlowTime = millis();

  lastUltrasonicTime = millis();

  lastOLEDTime = millis();

  lastWiFiCheck = millis();


  // ========================================================
  // TELEGRAM STARTUP
  // ========================================================

  if (WiFi.status() == WL_CONNECTED)
  {
    sendTelegramMessage(
      "🚰 Pipeline Leak Detection System Started.\n\n"
      "Pump: ON\n"
      "Leak threshold: 0.70 L/min\n"
      "Confirmation: 2 consecutive readings"
    );
  }


  Serial.println();

  Serial.println(
    "SYSTEM READY"
  );

  Serial.println(
    "Leak threshold = 0.70 L/min"
  );

  Serial.println(
    "Required readings = 2"
  );

  Serial.println(
    "======================================"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  // Flow calculation
  calculateFlow();


  // Leak detection
  checkLeak();


  // HC-SR04
  updateUltrasonic();


  // OLED
  updateOLED();


  // WiFi
  checkWiFi();


  // Website
  handleWebsite();


  delay(2);
}