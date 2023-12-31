#include <../lib/Time-master/TimeLib.h>
#include <../lib/pubsubclient-2.8/src/PubSubClient.h>
#include <../include/Creds/WifiCred.h>
#include <../include/Creds/HiveMQCred.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

#define DEVICE_NAME "Adler"
bool a = false;

// DS8
#define ONE_WIRE_BUS 0
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
DeviceAddress sensorAddress;
void sendIR(uint32_t);

enum Commands
{
  POWER = 0x10001,
  PLUS = 0x40004,
  MINUS = 0x20002,
  COUNT_DOWN = 0x400040,
  LED = 0x800080,
  MEIO = 0x200020,
  MODO = 0x100010,
  VENTILADOR = 0x80008,
  SLEEP1 = 0x10100,
  SLEEP2 = 0x20200,
  SLEEP3 = 0x40400
};

String getCommandName(uint32_t value)
{
  switch (value)
  {
  case POWER:
    return "POWER";
  case PLUS:
    return "PLUS";
  case MINUS:
    return "MINUS";
  case COUNT_DOWN:
    return "COUNT_DOWN";
  case LED:
    return "LED";
  case MEIO:
    return "MEIO";
  case MODO:
    return "MODO";
  case VENTILADOR:
    return "VENTILADOR";
  case SLEEP1:
    return "SLEEP1";
  case SLEEP2:
    return "SLEEP2";
  case SLEEP3:
    return "SLEEP3";
  default:
    return "UNKNOWN";
  }
}

void scheduleACShutdown(byte hours_till_shutdown)
{
  if (hours_till_shutdown > 12 || hours_till_shutdown == 0)
    return;

  sendIR(COUNT_DOWN);
  delay(200);
  for (size_t i = 0; i < hours_till_shutdown; i++)
  {
    sendIR(PLUS);
    delay(200);
  }
  sendIR(COUNT_DOWN);
}

#define serverPort 80 // Web server port (default is 80)
#define IR_SEND_PIN 13
#define DISABLE_CODE_FOR_RECEIVER
#include <IRremote.hpp>
ESP8266WebServer server(serverPort);
uint oldTime = 0;
double _current_temp = 0;

// TCP Definitions
WiFiClientSecure hive_client;
PubSubClient HiveMQ(hive_client);
#define DEBUG // comment for power efficiency

#pragma region OTA
void startOTA()
{
#ifdef DEBUG
  String type;
  // caso a atualização esteja sendo gravada na memória flash externa, então informa "flash"
  if (ArduinoOTA.getCommand() == U_FLASH)
    type = "flash";
  else                   // caso a atualização seja feita pela memória interna (file system), então informa "filesystem"
    type = "filesystem"; // U_SPIFFS
  // exibe mensagem junto ao tipo de gravação
  Serial.println("Start updating " + type);
#endif
}
// exibe mensagem
void endOTA()
{
#ifdef DEBUG
  Serial.println("\nEnd");
#endif
}
// exibe progresso em porcentagem
void progressOTA(unsigned int progress, unsigned int total)
{
#ifdef DEBUG
  Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
#endif
}

void errorOTA(ota_error_t error)
{
#ifdef DEBUG
  Serial.printf("Error[%u]: ", error);
  if (error == OTA_AUTH_ERROR)
    Serial.println("Auth Failed");
  else if (error == OTA_BEGIN_ERROR)
    Serial.println("Begin Failed");
  else if (error == OTA_CONNECT_ERROR)
    Serial.println("Connect Failed");
  else if (error == OTA_RECEIVE_ERROR)
    Serial.println("Receive Failed");
  else if (error == OTA_END_ERROR)
    Serial.println("End Failed");
#endif
}
#pragma endregion

struct Internals
{
  bool time_synced = false;
  uint boot_time = 0;
  bool ac_shutdown_scheduled = false;
  uint mqtt_update = 120;

  String toString()
  {
    String result = "time_synced: " + String(time_synced) + ";boot_time: " + String(boot_time) + ";life: " + String(now() - boot_time);
    return result;
  }
};

Internals control_variables;

struct Timers_Last_Values
{
  uint temperature = 0;
  uint sync_time = 0;
  uint mqtt_publish = 0;
  uint mqtt_reconnect = 0;
};
Timers_Last_Values Timers;

enum Temp_State
{
  UNKNOWNT = 0,
  WARMING = 1,
  COOLING = 2,
  IDLE = 3,
};

String getTempStateString(Temp_State state)
{
  switch (state)
  {
  case UNKNOWNT:
    return "Unknown";
  case WARMING:
    return "Warming";
  case COOLING:
    return "Cooling";
  case IDLE:
    return "Idle";
  default:
    return "Invalid State";
  }
}

struct Temperature
{
#define IDLE_TOLARANCE 0.5
  float history[20];
  byte _index = 0;
  Temp_State curr_state = Temp_State::UNKNOWNT;
  Temp_State old_state = Temp_State::UNKNOWNT;
  byte add(float temp)
  {
    history[_index] = temp;
    _index++;
    if (_index > 19)
    {
      _index = 0;
    }
    return _index;
  }

  byte getLastIndex(int offset = 0)
  {
    if (offset > 20)
      offset = 20;
    int d = _index + offset;

    if (d >= 20)
      return d - 20;
    else if (d == 0)
      return 19;
    else
      return d - 1;
  }

  Temp_State getTempState()
  {
    float _avg = 1;
    byte last_Index = getLastIndex(-1);
    // Serial.printf("Current Index: %d\n", last_Index);
    // Serial.println("--------------------------");
    for (size_t i = 0; i < 5; i++)
    {

      if (last_Index < i)
      {
        _avg *= history[20 - (i - (last_Index))];
        // Serial.printf(" %f : %d : %f \n", _avg, 20 - (i - (last_Index)), history[20 - (i - (last_Index))]);
      }
      else
      {
        _avg *= history[(last_Index)-i];
        // Serial.printf(" %f : %d : %f \n", _avg, (last_Index)-i, history[(last_Index)-i]);
      }
    }
    // Serial.println("--------------------------");
    if (_avg == 0)
      return Temp_State::UNKNOWNT;

    _avg = getAvg(5);
    // Serial.printf("%f : %f hist[%d] = %f\n", _avg, _avg - history[last_Index], last_Index, history[last_Index]);
    last_Index = getLastIndex();
    if (_avg - history[last_Index] >= IDLE_TOLARANCE && _avg - history[last_Index] <= -1 * IDLE_TOLARANCE)
    {
      if (_avg > 0)
        return Temp_State::WARMING;
      else
        return Temp_State::COOLING;
    }
    else
      return IDLE;
  }
  void reset()
  {
    for (size_t i = 0; i < 20; i++)
    {
      history[i] = 0;
    }
    _index = 0;
  }

  float getAvg(byte _size = 20)
  {
    if (_size > 20 || _size == 0)
      return -1;
    if (_index == 0)
      _index = 19;
    else
      _index--;
    float avg = 0;
    for (size_t i = 0; i < _size; i++)
    {

      if (_index < i)
        avg += history[20 - (i - _index)];
      else
        avg += history[_index - i];
    }
    if (_index == 19)
      _index = 0;
    else
      _index++;
    avg = avg / _size;
    return avg;
  }
  String report()
  {
    String s = "Held Values: ";
    if (_index == 0)
      _index = 19;
    else
      _index--;
    for (size_t i = 0; i < 20; i++)
    {
      s += "[";
      if (_index < i)
        s += history[20 - (i - _index)];
      else
        s += history[_index - i];

      s += "]";
    }
    if (_index == 19)
      _index = 0;
    else
      _index++;

    s += "\n State:";
    s += getTempState();
    s += "\n Index:";
    s += _index;
    s += "\n Last Index:";
    s += getLastIndex();
    s += "\n AVG: ";
    s += getAvg();
    s += "\n AVG [5]:";
    s += getAvg(5);
    return s;
  }
};

Temperature tempHandler;

#pragma region Server
void handleIRSend()
{
  if (server.hasArg("code"))
  {
    String argValue = server.arg("code");
    uint32_t code = strtoul(argValue.c_str(), NULL, HEX);
    sendIR(code);
    server.send(200, "text/plain", "Argument received: " + argValue);
  }
  Serial.printf("%d\n", server.args());
}
void handleRoot()
{
  String message =
      // ##$$page.html

      R"===(<!DOCTYPE html>
<html>

<head>
    <title>Adler IR Remote</title>
    <link rel="preload" href="page.css" as="style"/>
    <link rel="stylesheet" href="page.css">
    <link rel="stylesheet" href="https://maxcdn.bootstrapcdn.com/bootstrap/4.5.2/css/bootstrap.min.css">
    <script type="text/javascript" src="page.js"></script>
</head>

<body class="outer-div">
    <div class="card-container">
        <div class="card">
            <h2>Send IR Code</h2>
            <div>
                <input type="text"class="ir-input"  id="ir-code" /><button type="submit" onclick="sendIRCode()">Send</button>
            </div>
        </div>
        <div class="card">
            <h2>Room temperature:</h2>
            <div id="temperatureDiv">Loading...</div>
        </div>
        <div class="card">
            <h5>MQTT Status:</h5>
            <div id="mqttDiv">Loading...</div>
        </div>
        <div class="card">
            <h5>Next Action:</h5>
            <div id="next-actionDiv">Loading...</div>
        </div>
        <div class="card">
            <h5>Current Status:</h5>
            <div id="current-statusDiv">Loading...</div>
        </div>
        <div class="card">
            <div>
                <h6>Synced:</h6>
                <div id="synced-timeDiv">Loading...</div>
            </div>
            <div>
                <h6>Life:</h6>
                <div id="life-timeDiv">Loading...</div>
            </div>
            <div>
                <h6>Boot:</h6>
                <div id="boot-timeDiv">Loading...</div>
            </div>
        </div>
    </div>
</body>

</html>)==="
      // ##$$
      ;
  server.send(200, "text/html", message);
}

void handleJS()
{
  String message =
      // ##$$page.js

      R"===(var teststr = "temp:0.00;mqtt:0;time_synced: 1;boot_time: 1691759370;life: 2;status:Warming;";

function getDefineName(value) {
    switch (value) {
        case -4:
            return "MQTT_CONNECTION_TIMEOUT";
        case -3:
            return "MQTT_CONNECTION_LOST";
        case -2:
            return "MQTT_CONNECT_FAILED";
        case -1:
            return "MQTT_DISCONNECTED";
        case 0:
            return "MQTT_CONNECTED";
        case 1:
            return "MQTT_CONNECT_BAD_PROTOCOL";
        case 2:
            return "MQTT_CONNECT_BAD_CLIENT_ID";
        case 3:
            return "MQTT_CONNECT_UNAVAILABLE";
        case 4:
            return "MQTT_CONNECT_BAD_CREDENTIALS";
        case 5:
            return "MQTT_CONNECT_UNAUTHORIZED";
        default:
            return "Unknown";
    }
}
function test() {
    console.log(parseInfoData(teststr))
}
// Function to fetch and update the IR codes
function fetchInfoUpdates() {
    fetch('/info').then(response => response.text())
        .then(data => {
            parseInfoData(data);
        })


}
function parseInfoData(responseText) {
    const infoPairs = responseText.split(';');
    infoPairs.forEach(pair => {
        const [name, value] = pair.split(':');
        updateInfoData(name, value);
    });
}

function updateInfoData(name, value) {
    //console.log(name, value);
   // console.log(name,value);
    if (typeof (name) === undefined)
        return;
    if (name === "temp") {
        const temperatureDiv = document.getElementById('temperatureDiv');
        temperatureDiv.textContent = `${value} \u00B0C`;
    }
    else if (name === "mqtt") {
        var intvalue = parseInt(value);
        const mqttDiv = document.getElementById('mqttDiv');
        mqttDiv.textContent = getDefineName(intvalue);
        if (intvalue === 0) {
            mqttDiv.classList.add("mqtt-good");
            mqttDiv.classList.remove("mqtt-bad");
        }
        else {
            mqttDiv.classList.remove("mqtt-good");
            mqttDiv.classList.add("mqtt-bad");
        }
    }
    else if (name === "time_synced") {
        var intvalue = parseInt(value);
        const thisDiv = document.getElementById('synced-timeDiv');
        thisDiv.textContent = intvalue === 0 ? "Nope" : "Yup";
        if (intvalue === 1) {
            thisDiv.classList.add("mqtt-good");
            thisDiv.classList.remove("mqtt-bad");
        }
        else {
            thisDiv.classList.remove("mqtt-good");
            thisDiv.classList.add("mqtt-bad");
        }
    }
    else if (name === "life") {
        var intvalue = parseInt(value);
        const thisDiv = document.getElementById('life-timeDiv');
        thisDiv.textContent = formatTime(intvalue);

    }
    else if (name === "boot_time") {
        var date = new Date((parseInt(value) + 3 * 3600) * 1000);
        const thisDiv = document.getElementById('boot-timeDiv');
        const options = { year: '2-digit', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' };
        thisDiv.textContent = date.toLocaleString('pt-US', options);

    }
    else if (name === "status") {
        const thisDiv = document.getElementById('current-statusDiv');
        thisDiv.textContent = value;
        thisDiv.className= value;
    }
    

}

function formatTime(seconds) {
    if (seconds < 90) {
        return `${seconds} s`;
    } else if (seconds < 90 * 60) {
        const minutes = Math.floor(seconds / 60);
        return `${minutes} m`;
    } else {
        const hours = Math.floor(seconds / 3600);
        const remainingMinutes = Math.floor((seconds % 3600) / 60);

        if (remainingMinutes === 0) {
            return `${hours} h`;
        } else {
            const formattedMinutes = remainingMinutes < 10 ? `0${remainingMinutes}` : remainingMinutes;
            return `${hours} h ${formattedMinutes} m`;
        }
    }
}

// Fetch IR codes on page load and set an interval to update them periodically
document.addEventListener('DOMContentLoaded', function () {
    fetchInfoUpdates();
    setInterval(fetchInfoUpdates, 1000); // Update the codes every 5 seconds (adjust as needed)
    const irInput = document.getElementById("ir-code");

    irInput.addEventListener("keydown", function (event) {
        if (event.key === "Enter") {
            sendIRCode();
        }
    });
});

function updateTemperatureDisplay(temperature) {
    const temperatureDiv = document.getElementById('temperatureDiv');
    temperatureDiv.textContent = `${temperature} \u00B0C`;//+'&deg; C';
}

function sendIRCode() {
    const irInput = document.getElementById("ir-code");
    fetch('/send-ir?code=' + irInput.value)
        .then(response => response.text())
        .then(data => {
            console.log(data);
            // Optionally, update UI with response data
        })
        .catch(error => {
            console.error('Error:', error);
            // Handle error if needed
        });
    irInput.value = "";
}
)==="
      // ##$$
      ;
  server.send(200, "text/javascript", message);
}

void handleCSS()
{
  String message =
      // ##$$page.css

      R"===(@charset "utf-8";

.outer-div {
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100vh;
    margin: 0;
}
.card h2{
    margin: 5px;
}
.ir-input{
    border-radius: 30px;
    margin: 5px; 
    text-align:center;
}

/* Style the outer div */
.card-container {
    text-align: center;
    border: 1px solid #ccc;
    padding: 20px;
    border-radius: 10px;
}

/* Style the temperatureDiv */
#temperatureDiv {
    font-size: 24px;
    padding: 10px;
    margin: 5px;
    background-color: #f0f0f0;
    border-radius: 5px;
}

.mqtt-good {
    color: darkgreen;
}

.mqtt-bad {
    color: darkred;
}

.card {
    margin: 5px;
}
.Idle
{
    color: darkgreen;
}
.Cooling
{
    color: aqua;
}
.Warming
{
    color: darkred;
})==="
      // ##$$
      ;
  server.send(200, "text/css", message);
}

String getDefineName(int value)
{
  switch (value)
  {
  case -4:
    return "MQTT_CONNECTION_TIMEOUT";
  case -3:
    return "MQTT_CONNECTION_LOST";
  case -2:
    return "MQTT_CONNECT_FAILED";
  case -1:
    return "MQTT_DISCONNECTED";
  case 0:
    return "MQTT_CONNECTED";
  case 1:
    return "MQTT_CONNECT_BAD_PROTOCOL";
  case 2:
    return "MQTT_CONNECT_BAD_CLIENT_ID";
  case 3:
    return "MQTT_CONNECT_UNAVAILABLE";
  case 4:
    return "MQTT_CONNECT_BAD_CREDENTIALS";
  case 5:
    return "MQTT_CONNECT_UNAUTHORIZED";
  default:
    return "Unknown";
  }
}

void handleInfo()
{
  if (server.hasArg("tempvect"))
  {
    String response = tempHandler.report();
    server.send(200, "text/plain", response);
    return;
  }
  if (server.hasArg("setPubTime"))
  {
    String response = "nop";
    int newTimne = atoi(server.arg("setPubTime").c_str());
    if (newTimne > 0)
    {
      response = "new pub time: ";
      response += newTimne;
      response += " secs.";
      control_variables.mqtt_update = newTimne;
    }
    server.send(200, "text/plain", response);
    return;
  }
  String response = "temp:";
  response += String(_current_temp).substring(0, 4);
  response += ";mqtt:";
  response += HiveMQ.state();
  response += ";";
  response += control_variables.toString();
  response += ";";
  response += "status:";
  response += getTempStateString(tempHandler.getTempState());
  response += ";";
  server.send(200, "text/plain", response);
}
#pragma endregion

// Callback for payload at the mqtt topic
void HiveMQ_Callback(char *topic, byte *payload, unsigned int length)
{
  String incommingMessage = "";
  for (int i = 0; i < length; i++)
    incommingMessage += (char)payload[i];

  Serial.printf("MQTT::[%s]-->[%s]\n", topic, incommingMessage.c_str());
  // Only topic Shell will have valid commands and expect a response
  if (strcmp(topic, "Adler/Light") == 0)
  {
    incommingMessage == "1" ? digitalWrite(2, !HIGH) : digitalWrite(2, !LOW);
    incommingMessage == "HIGH" ? digitalWrite(2, !HIGH) : digitalWrite(2, !LOW);
    incommingMessage == "on" ? digitalWrite(2, !HIGH) : digitalWrite(2, !LOW);
  }
  else if (strcmp(topic, "Adler/Shutdown") == 0)
  {
    scheduleACShutdown(atoi(incommingMessage.c_str()));
    HiveMQ.publish("Adler/Response", "ack");
  }
   else if (strcmp(topic, "Adler/Send-IR") == 0)
  {
    sendIR(strtoul(incommingMessage.c_str(),NULL,HEX));
    String message = "Sent: ";
    message += getCommandName(strtoul(incommingMessage.c_str(),NULL,HEX));
    HiveMQ.publish("Adler/Response", message.c_str());
  }
}

bool getTime()
{
  bool result = false;
  Serial.println("Syncing Time Online");
  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://worldtimeapi.org/api/timezone/America/Bahia.txt"); // HTTP
  int httpCode = http.GET();
  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {
      Serial.printf("[HTTP] OK... code: %d\n", httpCode);
      String payload = http.getString();
      char str[payload.length() + 1];
      strcpy(str, payload.c_str());
      char *pch;
      pch = strtok(str, ":\n");
      int i = 0;
      int raw_offset = 0;
      while (pch != NULL)
      {
        i++;
        if (i == 23)
        {
          raw_offset = atoi(pch);
        }
        if (i == 27)
        {
          setTime(atoi(pch) + raw_offset);
        }
        // printf("%d: %s\n", i, pch);
        pch = strtok(NULL, ":\n");
      }
      String msg = "Time Synced ";
      msg += millis();
      msg += "ms from boot.";
      Serial.println(msg);
      result = true;
    }
    else
    {
      Serial.printf("[HTTP] Error code: %d\n", httpCode);
    }
  }
  else
  {
    Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  return result;
}

void sendIR(uint32_t Code)
{
  Serial.printf("Sending {0x%X}\n", Code);
  IrSender.sendPulseDistanceWidth(38, 9000, 4550, 600, 1700, 600, 550, Code, 24, PROTOCOL_IS_LSB_FIRST, 0, 0);
  String payload = "Sent: ";
  payload += getCommandName(Code);
  HiveMQ.publish("Adler/IR", payload.c_str());
  delay(1);
}

void setup()
{
  pinMode(ONE_WIRE_BUS, INPUT);
  pinMode(2, OUTPUT);
  tempHandler.reset();

  // tempSensor.begin();
  // tempSensor.getAddress(sensorAddress, 0);
  // tempSensor.setResolution(sensorAddress, 11);
  Serial.begin(115200);
  IrSender.begin(false);
  int sensors_found = tempSensor.getDeviceCount();
  Serial.printf("DS18 SENSORS: %d\n", sensors_found);
  for (size_t i = 0; i < sensors_found; i++)
  {
    Serial.printf("[%d] 0x", i);
    DeviceAddress addr;
    tempSensor.getAddress(addr, i);
    for (size_t j = 0; j < 8; j++)
    {
      if (addr[j] < 0x10)
        Serial.print("0");
      Serial.print(addr[j], HEX);
    }
    Serial.println("");
  }
  Serial.printf("\n");
  Serial.print("Getting Internet Acess");

  // Wifi Setup
  WiFi.mode(WIFI_STA); // Important to be explicitly connected as client

#ifdef DEVICE_NAME
  WiFi.hostname(DEVICE_NAME);
#endif

  WiFi.begin(WIFISSID, WIFIPASSWD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connection took (ms) : ");
  Serial.println(millis());
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  server.on("/", handleRoot);
  server.on("/page.js", handleJS);
  server.on("/page.css", handleCSS);
  server.on("/send-ir", handleIRSend); // Handle IR code transmission
  server.on("/info", handleInfo);      // Handle IR code transmission
  server.begin();
  Serial.println("HTTP server started");

  // OTA
  ArduinoOTA.setHostname(DEVICE_NAME);
  ArduinoOTA.setPort(8266);
  ArduinoOTA.onStart(startOTA);
  ArduinoOTA.onEnd(endOTA);
  ArduinoOTA.onProgress(progressOTA);
  ArduinoOTA.onError(errorOTA);
  ArduinoOTA.begin(true);

  hive_client.setInsecure();
  HiveMQ.setServer(MQTT_URL, MQTT_PORT);
  HiveMQ.setCallback(HiveMQ_Callback);

  control_variables.time_synced = getTime();
  control_variables.boot_time = now();

  if (HiveMQ.connect(DEVICE_NAME, MQTT_USER, MQTT_PASSWD))
    Serial.println("MQTT Connected");
  else
    Serial.printf("Can't Connect to MQTT Error Code : %d\n", HiveMQ.state());
  HiveMQ.subscribe("Adler/#");
}

void loop()
{
  ArduinoOTA.handle();
  server.handleClient();
  HiveMQ.loop();

  if (now() - Timers.temperature > 2)
  {
    Timers.temperature = now();
    tempSensor.requestTemperatures();
    _current_temp = tempSensor.getTempCByIndex(0);
  }
  if (now() - Timers.sync_time > 300 && !control_variables.time_synced)
  {
    Timers.sync_time = now();
    control_variables.time_synced = getTime();
  }
  if (now() - Timers.mqtt_publish > control_variables.mqtt_update)
  {
    Timers.mqtt_publish = now();
    String temp = String(_current_temp);
    tempHandler.add(_current_temp);
    HiveMQ.publish("Adler/Temperature", temp.c_str());
  }
  if (now() - Timers.mqtt_reconnect > 300 && !HiveMQ.connected())
  {
    Timers.mqtt_reconnect = now();
    HiveMQ.unsubscribe("Adler/#");
    if (HiveMQ.connect(DEVICE_NAME, MQTT_USER, MQTT_PASSWD))
      Serial.println("MQTT Connected");
    else
      Serial.printf("Can't Connect to MQTT Error Code : %d\n", HiveMQ.state());
    HiveMQ.subscribe("Adler/#");
  }
  if (control_variables.time_synced && hour() == 4 && !control_variables.ac_shutdown_scheduled)
  {
    scheduleACShutdown(3);
    control_variables.ac_shutdown_scheduled = true;
  }
  if (hour() != 4 && control_variables.ac_shutdown_scheduled)
  {
    control_variables.ac_shutdown_scheduled = false;
  }
}
