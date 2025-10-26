#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <Adafruit_Sensor.h>
#include <MQUnifiedsensor.h>
#include "esp_task_wdt.h"

// ===== CONFIGURACIÓN WiFi =====
const char* ssid = "TU_SSID";
const char* password = "TU_PASSWORD";

// ===== CONFIGURACIÓN THINGSPEAK =====
const char* server = "http://api.thingspeak.com/update";
String apiKey = "TU_API_KEY";

// ===== CONFIGURACIÓN DHT22 =====
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ===== CONFIGURACIÓN MQ-135 =====
#define PIN_MQ135 34
#define VOLTAGE_RESOLUTION 3.3
#define ADC_BIT_RESOLUTION 12
#define RATIO_CLEAN_AIR 3.6
MQUnifiedsensor MQ135("ESP32", VOLTAGE_RESOLUTION, ADC_BIT_RESOLUTION, PIN_MQ135, "MQ-135");

// ===== CONFIGURACIÓN GUVA-S12SD =====
#define PIN_GUVA 35 // Entrada analógica para sensor UV

// ===== VARIABLES DE TIEMPO =====
unsigned long previousDHT = 0;
unsigned long previousMQ135 = 0;
unsigned long previousSend = 0;

const unsigned long intervalDHT = 30000;   // 30 segundos
const unsigned long intervalMQ135 = 120000; // 120 segundos
const unsigned long intervalSend = 900000;  // 15 minutos

// ===== VARIABLES DE MONITOREO =====
float temperature = 0;
float humidity = 0;
float airQuality = 0;
float uvLevel = 0;

int erroresComunicacion = 0;

// ===== FUNCIONES DE MONITOREO DE SISTEMA =====
float getCPUFreq() {
  return (float)getCpuFrequencyMhz();
}

float getFreeRAM() {
  return (float)ESP.getFreeHeap() / 1024.0;
}

float getSupplyVoltage() {
  return 3.3; // valor simulado
}

// ===== DETECCIÓN DE REINICIO =====
void detectarReinicio() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Motivo del último reinicio: ");
  switch (reason) {
    case ESP_RST_POWERON:   Serial.println("Encendido normal (POWERON)"); break;
    case ESP_RST_SW:        Serial.println("Reinicio por software (SW)"); break;
    case ESP_RST_PANIC:     Serial.println("Fallo crítico (PANIC)"); break;
    case ESP_RST_INT_WDT:   Serial.println("Watchdog por interrupción (INT_WDT)"); break;
    case ESP_RST_TASK_WDT:  Serial.println("Watchdog por tarea (TASK_WDT)"); break;
    case ESP_RST_BROWNOUT:  Serial.println("Voltaje bajo (BROWNOUT)"); break;
    default:                Serial.println("Otro tipo de reinicio"); break;
  }
}

// ===== VERIFICAR MEMORIA =====
void verificarMemoria() {
  if (ESP.getFreeHeap() < 20000) { // Umbral de 20 KB
    Serial.println("⚠️ Memoria baja detectada. Reiniciando para liberar recursos...");
    delay(2000);
    ESP.restart();
  }
}

// ===== CONFIGURACIÓN INICIAL =====
void setup() {
  Serial.begin(115200);
  detectarReinicio();

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado!");
  Serial.print("Dirección IP: ");
  Serial.println(WiFi.localIP());

  dht.begin();
  MQ135.init();
  MQ135.setRegressionMethod(1); // método lineal
  MQ135.setA(110.47); MQ135.setB(-2.862); // calibración para CO2

  // Configurar Watchdog (10 segundos)
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  Serial.println("Sistema listo. Iniciando monitoreo...");
}

// ===== LOOP PRINCIPAL =====
void loop() {
  esp_task_wdt_reset();  // Reinicia el watchdog
  unsigned long currentMillis = millis();

  // --- Lectura de DHT22 cada 30 s ---
  if (currentMillis - previousDHT >= intervalDHT) {
    previousDHT = currentMillis;
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("❌ Error de lectura DHT22");
    } else {
      Serial.printf("🌡️ DHT22 -> Temp: %.2f °C | Humedad: %.2f %%\n", temperature, humidity);
    }
  }

  // --- Lectura de MQ135 cada 120 s ---
  if (currentMillis - previousMQ135 >= intervalMQ135) {
    previousMQ135 = currentMillis;
    MQ135.update();
    airQuality = MQ135.readSensor();
    Serial.printf("💨 MQ135 -> Calidad de aire (CO2 eq): %.2f ppm\n", airQuality);
  }

  // --- Lectura continua de GUVA-S12SD ---
  int uvAnalog = analogRead(PIN_GUVA);
  uvLevel = (uvAnalog / 4095.0) * 3.3; // conversión a voltaje
  Serial.printf("☀️ GUVA -> Voltaje UV: %.3f V\n", uvLevel);

  // --- Monitoreo de recursos del sistema ---
  Serial.printf("🧠 CPU: %.0f MHz | RAM libre: %.2f KB | Voltaje: %.2f V\n",
                getCPUFreq(), getFreeRAM(), getSupplyVoltage());

  verificarMemoria();

  // --- Envío de datos a ThingSpeak cada 15 minutos ---
  if (currentMillis - previousSend >= intervalSend) {
    previousSend = currentMillis;
    enviarDatosThingSpeak();
  }

  delay(1000);
}

// ===== FUNCIÓN PARA ENVIAR DATOS A THINGSPEAK =====
void enviarDatosThingSpeak() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    String url = String(server) + "?api_key=" + apiKey;
    url += "&field1=" + String(temperature, 2);
    url += "&field2=" + String(humidity, 2);
    url += "&field3=" + String(airQuality, 2);
    url += "&field4=" + String(uvLevel, 2);
    url += "&field5=" + String(getCPUFreq(), 0);
    url += "&field6=" + String(getFreeRAM(), 2);
    url += "&field7=" + String(getSupplyVoltage(), 2);

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
      Serial.printf("✅ Datos enviados a ThingSpeak (HTTP %d)\n", httpCode);
      erroresComunicacion = 0;
    } else {
      erroresComunicacion++;
      Serial.printf("❌ Error de comunicación #%d: %s\n", erroresComunicacion, http.errorToString(httpCode).c_str());

      if (erroresComunicacion >= 3) {
        Serial.println("⚠️ 3 errores seguidos. Reiniciando WiFi...");
        WiFi.disconnect(true);
        delay(1000);
        WiFi.reconnect();
        if (erroresComunicacion >= 5) {
          Serial.println("💥 Error persistente. Reiniciando sistema...");
          ESP.restart();
        }
      }
    }

    http.end();
  } else {
    Serial.println("📡 WiFi desconectado. Intentando reconectar...");
    WiFi.reconnect();
  }
}
