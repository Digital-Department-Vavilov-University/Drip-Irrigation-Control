#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "UniversalTelegramBot.h"
#include "ArduinoJson.h"
#include <ThingSpeak.h>
#include <WebServer.h>
#include <EEPROM.h>

#define RELAY_WATER 25    // Пин для управления поливом
#define RELAY_HEAT 26     // Пин для управления обогревом почвы

bool relayWaterState = false;
bool relayHeatState = false;
bool autoWater = false;
bool autoHeat = false;

// Пороговые значения для автоматического управления
float tempMin = 15, tempMax = 25;    // Для полива
float heatOnTemp = 10, heatOffTemp = 18; // Для обогрева
float humMin = 30, humMax = 70;

#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 32

const char* DEFAULT_SSID = "****";
const char* DEFAULT_PASS = "****";

const char* BOTtoken = "***";
const char* CHAT_ID = "***";

const char* thingspeakApiKey = "***";
unsigned long thingspeakChannelID = ****;
WiFiClient thingspeakClient;

WebServer configServer(80);
const char* AP_SSID = "Teplica_Controller";
bool configMode = false;

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

const int botRequestDelay = 1000;
const long sensorUpdateInterval = 60000; // 1 минута
unsigned long lastTimeBotRan = 0;
unsigned long lastSensorUpdate = 0;

// Пины для датчика почвы
#define TEMP_PIN 34
#define HUM_PIN 35
const float TEMP_COEFF = 0.1;
const float HUM_COEFF = 0.2;

const char* configForm = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>WiFi Configuration</title>
  <meta charset="utf-8">
  <style>
    body { font-family: Arial; text-align: center; margin-top: 50px; }
    input { margin: 10px; padding: 5px; }
  </style>
</head>
<body>
  <h1>Настройка Wi-Fi</h1>
  <form action="/save" method="POST">
    <input type="text" name="ssid" placeholder="SSID" required><br>
    <input type="password" name="pass" placeholder="Password" required><br>
    <input type="submit" value="Сохранить и перезагрузить">
  </form>
</body>
</html>
)rawliteral";

void saveCredentials(const String& ssid, const String& pass) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(SSID_ADDR, ssid);
  EEPROM.put(PASS_ADDR, pass);
  EEPROM.commit();
  EEPROM.end();
}

void loadCredentials(String& ssid, String& pass) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ssid);
  EEPROM.get(PASS_ADDR, pass);
  EEPROM.end();

  if (ssid.length() == 0 || pass.length() == 0) {
    ssid = DEFAULT_SSID;
    pass = DEFAULT_PASS;
  }
}

void handleRoot() {
  configServer.send(200, "text/html", configForm);
}

void handleSave() {
  String newSSID = configServer.arg("ssid");
  String newPass = configServer.arg("pass");

  if (newSSID.length() == 0 || newPass.length() == 0) {
    configServer.send(400, "text/plain", "Error: Empty fields!");
    return;
  }

  saveCredentials(newSSID, newPass);
  configServer.send(200, "text/plain", "Настройки сохранены! Устройство перезагрузится...");
  delay(2000);
  ESP.restart();
}

void setupWiFi() {
  String ssid, pass;
  loadCredentials(ssid, pass);

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("Подключение к WiFi ");
  Serial.println(ssid);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 60000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nНе удалось подключиться! Запуск точки доступа...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    Serial.print("AP SSID: ");
    Serial.println(AP_SSID);
    Serial.print("IP адрес: ");
    Serial.println(WiFi.softAPIP());

    configServer.on("/", handleRoot);
    configServer.on("/save", handleSave);
    configServer.begin();
    configMode = true;
  } else {
    Serial.println("\nПодключено! IP: " + WiFi.localIP().toString());
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    ThingSpeak.begin(thingspeakClient);
    bot.sendMessage(CHAT_ID, "Контроллер теплицы запущен!", "");
  }
}

String getReadings() {
  int rawTemp = analogRead(TEMP_PIN);
  int rawHum = analogRead(HUM_PIN);
  
  float temperature = (rawTemp / 4095.0 * 3.3) / TEMP_COEFF;
  float humidity = (rawHum / 4095.0 * 3.3) / HUM_COEFF;

  return "🌡 Температура почвы: " + String(temperature) + " °C\n" +
         "💧 Влажность почвы: " + String(humidity) + " %\n" +
         "💦 Полив: " + String(relayWaterState ? "ВКЛ" : "ВЫКЛ") + (autoWater ? " (АВТО)" : " (РУЧН)") + "\n" +
         "🔥 Обогрев: " + String(relayHeatState ? "ВКЛ" : "ВЫКЛ") + (autoHeat ? " (АВТО)" : " (РУЧН)");
}

void sendSensorData() {
  String readings = getReadings();
  bot.sendMessage(CHAT_ID, readings, "");
}

void sendToThingSpeak(float temperature, float humidity) {
  if (WiFi.status() != WL_CONNECTED) return;

  ThingSpeak.setField(1, temperature);
  ThingSpeak.setField(2, humidity);
  ThingSpeak.setField(3, relayWaterState);
  ThingSpeak.setField(4, relayHeatState);
  ThingSpeak.writeFields(thingspeakChannelID, thingspeakApiKey);
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Доступ запрещен!", "");
      continue;
    }

    if (text.startsWith("/start")) {
      String welcome = "Привет, " + from_name + "!\n";
      welcome += "🌱 Команды управления теплицей:\n";
      welcome += "/readings - Показания датчиков\n";
      welcome += "/water_on /water_off - Ручное управление поливом\n";
      welcome += "/heat_on /heat_off - Ручное управление обогревом\n";
      welcome += "/set_temp min max - пороги температуры для полива\n";
      welcome += "/set_hum min max - пороги влажности для полива\n";
      welcome += "/set_heat on off - пороги температуры для обогрева\n";
      welcome += "/auto_off - отключить все авторежимы\n";
      welcome += "/status - Статус системы\n";
      bot.sendMessage(chat_id, welcome, "");
    }
    else if (text == "/readings") {
      bot.sendMessage(chat_id, getReadings(), "");
    }
    else if (text == "/water_on") {
      autoWater = false;
      digitalWrite(RELAY_WATER, HIGH);
      relayWaterState = true;
      bot.sendMessage(chat_id, "💦 Полив включён вручную", "");
    }
    else if (text == "/water_off") {
      autoWater = false;
      digitalWrite(RELAY_WATER, LOW);
      relayWaterState = false;
      bot.sendMessage(chat_id, "💦 Полив выключен вручную", "");
    }
    else if (text == "/heat_on") {
      autoHeat = false;
      digitalWrite(RELAY_HEAT, HIGH);
      relayHeatState = true;
      bot.sendMessage(chat_id, "🔥 Обогрев включён вручную", "");
    }
    else if (text == "/heat_off") {
      autoHeat = false;
      digitalWrite(RELAY_HEAT, LOW);
      relayHeatState = false;
      bot.sendMessage(chat_id, "🔥 Обогрев выключен вручную", "");
    }
    else if (text == "/auto_off") {
      autoWater = false;
      autoHeat = false;
      bot.sendMessage(chat_id, "⚙️ Все автоматические режимы отключены", "");
    }
    else if (text.startsWith("/set_temp ")) {
      int space1 = text.indexOf(' ');
      int space2 = text.indexOf(' ', space1 + 1);
      if (space1 > 0 && space2 > 0) {
        tempMin = text.substring(space1 + 1, space2).toFloat();
        tempMax = text.substring(space2 + 1).toFloat();
        autoWater = true;
        bot.sendMessage(chat_id, "🌡 Авторежим полива по температуре: " + String(tempMin) + " - " + String(tempMax) + " °C", "");
      }
    }
    else if (text.startsWith("/set_hum ")) {
      int space1 = text.indexOf(' ');
      int space2 = text.indexOf(' ', space1 + 1);
      if (space1 > 0 && space2 > 0) {
        humMin = text.substring(space1 + 1, space2).toFloat();
        humMax = text.substring(space2 + 1).toFloat();
        autoWater = true;
        bot.sendMessage(chat_id, "💧 Авторежим полива по влажности: " + String(humMin) + " - " + String(humMax) + " %", "");
      }
    }
    else if (text.startsWith("/set_heat ")) {
      int space1 = text.indexOf(' ');
      int space2 = text.indexOf(' ', space1 + 1);
      if (space1 > 0 && space2 > 0) {
        heatOnTemp = text.substring(space1 + 1, space2).toFloat();
        heatOffTemp = text.substring(space2 + 1).toFloat();
        autoHeat = true;
        bot.sendMessage(chat_id, "🔥 Авторежим обогрева: включить при " + String(heatOnTemp) + "°C, выключить при " + String(heatOffTemp) + "°C", "");
      }
    }
    else if (text == "/status") {
      bot.sendMessage(chat_id, getReadings(), "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  pinMode(RELAY_WATER, OUTPUT);
  pinMode(RELAY_HEAT, OUTPUT);
  digitalWrite(RELAY_WATER, LOW);
  digitalWrite(RELAY_HEAT, LOW);

  pinMode(TEMP_PIN, INPUT);
  pinMode(HUM_PIN, INPUT);

  setupWiFi();
}

void loop() {
  if (configMode) {
    configServer.handleClient();
    return;
  }

  if (millis() - lastTimeBotRan > botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }

  if (millis() - lastSensorUpdate > sensorUpdateInterval) {
    int rawTemp = analogRead(TEMP_PIN);
    int rawHum = analogRead(HUM_PIN);
    float temperature = (rawTemp / 4095.0 * 3.3) / TEMP_COEFF;
    float humidity = (rawHum / 4095.0 * 3.3) / HUM_COEFF;

    // Автоматическое управление поливом
    if (autoWater) {
      if ((temperature > tempMax || humidity > humMax) && !relayWaterState) {
        digitalWrite(RELAY_WATER, HIGH);
        relayWaterState = true;
        bot.sendMessage(CHAT_ID, "💦 Авто: Полив включен (T=" + String(temperature) + "°C, H=" + String(humidity) + "%)", "");
      } 
      else if ((temperature < tempMin && humidity < humMin) && relayWaterState) {
        digitalWrite(RELAY_WATER, LOW);
        relayWaterState = false;
        bot.sendMessage(CHAT_ID, "💦 Авто: Полив выключен (T=" + String(temperature) + "°C, H=" + String(humidity) + "%)", "");
      }
    }

    // Автоматическое управление обогревом
    if (autoHeat) {
      if (temperature < heatOnTemp && !relayHeatState) {
        digitalWrite(RELAY_HEAT, HIGH);
        relayHeatState = true;
        bot.sendMessage(CHAT_ID, "🔥 Авто: Обогрев включен (T=" + String(temperature) + "°C)", "");
      } 
      else if (temperature > heatOffTemp && relayHeatState) {
        digitalWrite(RELAY_HEAT, LOW);
        relayHeatState = false;
        bot.sendMessage(CHAT_ID, "🔥 Авто: Обогрев выключен (T=" + String(temperature) + "°C)", "");
      }
    }

    sendSensorData();
    sendToThingSpeak(temperature, humidity);
    lastSensorUpdate = millis();
  }
}