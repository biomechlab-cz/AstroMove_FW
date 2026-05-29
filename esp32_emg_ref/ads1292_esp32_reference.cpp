#include "protocentralAds1292r.h"
#include "ecgRespirationAlgo.h"
#include "defines.h"
#include "config_helpers.h"
#include "log_utils.h"

#include <Preferences.h>
#include <atomic>
#include <Esp.h>
#include <QMC5883LCompass.h>
#include <LSM6.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <mutex>
#include <semphr.h>
#include <ESP32Time.h>
#include <queue.h>
#include <FS.h>
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
using namespace std;
/// INSERT DEVICE ID HERE
#define DEVICE_ID 4

// #include <ESP1588.h> //  USE THIS for better timing accuracy

#ifdef ESP32
#include <SPIFFS.h>
#endif
#include <HTTPClient.h>

ads1292r ADS1292R;
ads1292OutputValues adsOUTPUTvalues;
ConfigHelpers configHelpers = ConfigHelpers();
QMC5883LCompass compass;
LSM6 imu;

static portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

char ap_name[50] = {0};        // zero-initialize
char server_address[20] = {0}; // same for these
char participant[50] = {0};
char position[50] = {0};

int serverUnavailableRestartCounter;
WiFiManagerParameter custom_server_address("server", "sever ip address", server_address, 20);
WiFiManagerParameter custom_participant("participant", "participant", participant, 50);
WiFiManagerParameter custom_position("position", "position", position, 50);

WiFiManager wifiManager;
WiFiClient client;

Preferences preferences;
// ---- TASKS ----
TaskHandle_t Task1Reading;
TaskHandle_t Task2ReadingSupport;
TaskHandle_t Task3Sending;

QueueHandle_t dataQueue;

SemaphoreHandle_t adcReadySem = xSemaphoreCreateBinary();
SemaphoreHandle_t readSupportDataSem = xSemaphoreCreateBinary();

atomic<int16_t> supportDataWriteIndex(0);
atomic<int16_t> emgDataWriteIndex(0);

struct DataStruct dataStructNow;
struct InitialDataStructure initialData;

// ----TIME----
ESP32Time ESP32_time;

bool startMeasuring = false;
int port = 8888;

bool initialDataSent = false;
bool shouldSaveConfig = false;

/// Sends directly to serial if availible, if not save to log file
void generalPrint(const char *format, ...)
{
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  if (Serial)
  {
    Serial.println(buf);
  }
  else
  {
    // logToFile expects a format string and variable arguments
    logToFile("%s", buf);
  }
}

void saveConfigCallback()
{
  configHelpers.writeConfigFile(&preferences, server_address, participant, position, &custom_server_address, &custom_participant, &custom_position);
}

void IRAM_ATTR myInterruptCall(void *)
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(adcReadySem, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
void getApName()
{
  snprintf(ap_name, sizeof(ap_name), "ESP32AP_V2_%d_B", DEVICE_ID);
}

uint8_t readBatteryLevel()
{
  return map(analogRead(1), 1400, 2600, 0, 100);
}

void dataReadingSecondaryTask(void *pvParameters);
void dataReadingTask(void *pvParameters);
void dataSendingTask(void *pvParameters);

void setup()
{
  Serial.begin(115200);
  Serial.println();

  // Wait for a serial connection for up to 5 seconds.
  // This is crucial for ESP32-S3 and other boards with native USB.
  // It prevents logs from being cleared if no monitor is attached.
  for (int i = 0; i < 50 && !Serial; i++)
  {
    delay(100);
  }

  Serial.setDebugOutput(true);

  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
  }
  else
  {
    if (Serial)
    { // Only print and clear logs if a serial monitor is connected
      printAndClearLogs();
    }
    else
    {
      logToFile("No serial monitor connected, skipping log print.");
    }
  }

  generalPrint("Begin initialization");
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
  dataQueue = xQueueCreate(10, sizeof(struct DataStruct)); // Queue to hold data batches
  preferences.begin("main", false);

  configHelpers.readConfigFile(&preferences, server_address, participant, position);

  custom_server_address.setValue(server_address, sizeof(server_address));
  custom_participant.setValue(participant, sizeof(participant));
  custom_position.setValue(position, sizeof(position));

  WiFi.mode(WIFI_STA);

  // reset settings - for testing
  // wifiManager.resetSettings();

  // ----- Reset prefs for testing ----
  // Uncomment to erase stored wifi and positions
  // Do only once, then comment it again and run, esp will forget wifi and position
  // wifiManager.erase();
  // preferences.clear();

  wifiManager.setConnectTimeout(5);
  wifiManager.setConfigPortalTimeout(60);

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setBreakAfterConfig(true);

  // add all your parameters here
  wifiManager.addParameter(&custom_server_address);
  wifiManager.addParameter(&custom_participant);
  wifiManager.addParameter(&custom_position);
  getApName();

  // This is used when only the server is missing, but wifi is still available
  bool startWithAP = preferences.getBool(START_CONFIG_SERVER, false);
  if (startWithAP)
  {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(500);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(500);
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(500);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(500);
    preferences.putBool(START_CONFIG_SERVER, false);
    generalPrint("starting config portal");
    wifiManager.startConfigPortal(ap_name);
  }

  configHelpers.readConfigFile(&preferences, server_address, participant, position);
  if (server_address[0] == '\0')
  {
    generalPrint("No server address found, clearing wifi manager");
    wifiManager.erase();
  }

  if (!wifiManager.autoConnect(ap_name))
  {
    generalPrint("failed to connect and hit timeout");
    delay(500);
    // reset and try again, or maybe put it to deep sleep
    preferences.end();
    ESP.restart();
  }

  /// ConnectedToWifi
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(1000);
  digitalWrite(GREEN_LED_PIN, LOW);

  initialData.device_id = DEVICE_ID;
  strcpy(initialData.participant, participant);
  strcpy(initialData.position, position);

  //---- TIME ----  //
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeInfo;

  // IF this fails, time starts at posix zero 1970
  if (getLocalTime(&timeInfo))
  {
    ESP32_time.setTimeStruct(timeInfo);
  }
  else
  {
    generalPrint("Failed to obtain time");
  }

  generalPrint("socketIO setup on: %s:%d\n", server_address, port);
  generalPrint("data size: %u", sizeof(DataStruct));

  generalPrint("I2C imu and compass setup");

  Wire.begin(SDA_pin, SCL_pin);

  compass.init();
  imu.init();
  imu.enableDefault();

  // ----------------- sockets under -----------------
  generalPrint("SPI and ADS1292R setup");
  SPI.begin(VSPI_SCLK, VSPI_MISO, VSPI_MOSI, VSPI_SS);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE1);           // CPOL = 0, CPHA = 1
  SPI.setClockDivider(SPI_CLOCK_DIV16); // Selecting 1Mhz clock for SPI

  pinMode(ADS1292_DRDY_PIN, INPUT);
  pinMode(ADS1292_CS_PIN, OUTPUT);
  pinMode(ADS1292_START_PIN, OUTPUT);
  pinMode(ADS1292_PWDN_PIN, OUTPUT);

  ADS1292R.ads1292Init(ADS1292_CS_PIN, ADS1292_PWDN_PIN, ADS1292_START_PIN, ADS1292_DRDY_PIN);
  attachInterruptArg(ADS1292_DRDY_PIN, myInterruptCall, NULL, FALLING);

  /// before starting measuring threads
  /// start the socket connection and ask for config
  // wait to get start command from server
  //  while (startMeasuring == false)
  //  {
  //  }

  Serial.println("Start ADS measuring thread");
  xTaskCreatePinnedToCore(
      dataReadingTask,          /* Task function. */
      "dataReadingTask",        /* name of task. */
      10000,                    /* Stack size of task */
      NULL,                     /* parameter of the task */
      configMAX_PRIORITIES - 1, /* priority of the task */
      &Task1Reading,            /* Task handle to keep track of created task */
      1);

  Serial.println("Start secondary data reading thread");

  xTaskCreatePinnedToCore(
      dataReadingSecondaryTask,   /* Task function. */
      "dataReadingSecondaryTask", /* name of task. */
      10000,                      /* Stack size of task */
      NULL,                       /* parameter of the task */
      configMAX_PRIORITIES - 2,   /* priority of the task */
      &Task2ReadingSupport,       /* Task handle to keep track of created task */
      1);

  Serial.println("Start data sending thread");

  xTaskCreatePinnedToCore(
      dataSendingTask,          /* Task function. */
      "dataSendingTask",        /* name of task. */
      10000,                    /* Stack size of task */
      NULL,                     /* parameter of the task */
      configMAX_PRIORITIES - 3, /* priority of the task */
      &Task3Sending,            /* Task handle to keep track of created task */
      0);

  digitalWrite(RED_LED_PIN, LOW);

  generalPrint("Initialization is done");
}

void dataReadingTask(void *pvParameters)
{
  generalPrint("Data reading task started");

  while (true)
  {
    if (xSemaphoreTake(adcReadySem, portMAX_DELAY) == pdTRUE)
    {
      boolean ret = ADS1292R.getAds1292EcgAndRespirationSamples(ADS1292_DRDY_PIN, ADS1292_CS_PIN, &adsOUTPUTvalues);
      if (ret == true)
      {
        // Vref voltage 2.42, default gain is 6
        // so calculating voltage per LSB
        // LSB = (2 x VREF) / Gain / (2 ^ 24 - 1)

        int32_t emgValue = (int32_t)(adsOUTPUTvalues.sDaqVals[1]);
        if (emgDataWriteIndex < CHUNK_SEND_SIZE)

          dataStructNow.emg_data_arr[emgDataWriteIndex] = emgValue;

        if (emgDataWriteIndex % 10 == 0)
        {
          xSemaphoreGive(readSupportDataSem);
        }
        emgDataWriteIndex++;
      }
    }

    if (emgDataWriteIndex == CHUNK_SEND_SIZE)
    {
      // DataStruct *newAllocation = (DataStruct *)malloc(sizeof(DataStruct));
      emgDataWriteIndex = 0;
      supportDataWriteIndex = 0;
      // The queue send functions internally does memcpy of the data
      if (xQueueSend(dataQueue, (void *)&dataStructNow, portMAX_DELAY) != pdPASS)
      {
        generalPrint("ERROR: Data could not queue");
      }
    }
  }
}
void dataReadingSecondaryTask(void *pvParameters)
{
  struct timeval tv;
  while (true)
  {
    if (xSemaphoreTake(readSupportDataSem, portMAX_DELAY) == pdTRUE)
    {
      compass.read();
      imu.read();
      tm timeStruct = ESP32_time.getTimeStruct();
      gettimeofday(&tv, NULL);

      dataStructNow.compass_x[supportDataWriteIndex] = compass.getX();
      dataStructNow.compass_y[supportDataWriteIndex] = compass.getY();
      dataStructNow.compass_z[supportDataWriteIndex] = compass.getZ();
      dataStructNow.imu_gyro_x[supportDataWriteIndex] = imu.g.x;
      dataStructNow.imu_gyro_y[supportDataWriteIndex] = imu.g.y;
      dataStructNow.imu_gyro_z[supportDataWriteIndex] = imu.g.z;
      dataStructNow.imu_acc_x[supportDataWriteIndex] = imu.a.x;
      dataStructNow.imu_acc_y[supportDataWriteIndex] = imu.a.y;
      dataStructNow.imu_acc_z[supportDataWriteIndex] = imu.a.z;
      dataStructNow.time[supportDataWriteIndex] = tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL);

      supportDataWriteIndex++;
    }
  }
}
void dataSendingTask(void *pvParameters)
{
  struct SocketSendStruct structureToSend;
  generalPrint("Data sending task started");
  generalPrint("Connecting to server %s : %d", server_address, port);
  generalPrint("With device  %d,participant: %s position: %s", DEVICE_ID, participant, position);
  int connectionFailCounter = 0;
  IPAddress ip;

  while (true)
  {
    if (!ip.fromString(server_address))
    {
      generalPrint("Bad server IP: '%s'", server_address);
      // decide: start portal or restart
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!client.connect(ip, port))
    {
      generalPrint("Connection to %s:%d failed.", server_address, port);
      digitalWrite(RED_LED_PIN, HIGH);
      delay(200);
      digitalWrite(RED_LED_PIN, LOW);
      delay(100);
      connectionFailCounter++;
      if (connectionFailCounter > SERVER_CONNECTION_FAILS_MAX_COUNT)
      {
        int restartCounter = preferences.getInt(SERVER_UNAVAILABLE_RESTART_COUNTER, 0);
        generalPrint("Server not found restart counter: %d", restartCounter);

        if (restartCounter > MAX_RESTART_COUNT_TO_SETUP_AP)
        {
          digitalWrite(RED_LED_PIN, HIGH);
          digitalWrite(GREEN_LED_PIN, HIGH);
          delay(1000);
          preferences.putInt(SERVER_UNAVAILABLE_RESTART_COUNTER, 0);
          preferences.putBool(START_CONFIG_SERVER, true);
          generalPrint("Connection failed too many times, restarting and forcing AP start.");
          preferences.end();
          ESP.restart();
        }
        else
        {
          restartCounter += 1;
          generalPrint("Incrementing server unavailable counter to %d", restartCounter);
          preferences.putInt(SERVER_UNAVAILABLE_RESTART_COUNTER, restartCounter);
        }

        generalPrint("Connection failed too many times, restarting.");
        preferences.end();
        ESP.restart();
      }
      continue;
    }
    if (initialDataSent == false)
    {
      generalPrint("Sending initial data.");
      structureToSend.isInitialData = 1; // 1 for initial data
      structureToSend.dataUnion.initialData = initialData;
      client.write((char *)&structureToSend, sizeof(SocketSendStruct));
      client.flush();
      initialDataSent = true;
      char ack[50] = {0};
      client.readBytes(ack, sizeof(ack) - 1);
      generalPrint("Received initial data ACK: %s", ack);
    }

    generalPrint("Connected to server, starting data transmission.");
    digitalWrite(GREEN_LED_PIN, HIGH);
    if (client)
      structureToSend.isInitialData = 0; // 0 for false
    // Send data to the server
    while (client.connected())
    { // loop while the client's connected
      if (xQueueReceive(dataQueue, &structureToSend.dataUnion.mainData, portMAX_DELAY) == pdPASS)
      {
        structureToSend.batteryLevel = readBatteryLevel();
        client.write((char *)&structureToSend, sizeof(SocketSendStruct));
        client.flush();
      }
    }
    digitalWrite(GREEN_LED_PIN, LOW);
    generalPrint("Client disconnected.");
  }
}
void loop()
{
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
