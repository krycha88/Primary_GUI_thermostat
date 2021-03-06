#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

#include <EEPROM.h>
#include <DoubleResetDetector.h> //Bilioteka by Stephen Denne

#define SUPLADEVICE_CPP
#include <SuplaDevice.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

#include "supla_settings.h"
#include "supla_eeprom.h"
#include "supla_web_server.h"
#include "supla_board_settings.h"

#include "thermostat.h"

extern "C" {
#include "user_interface.h"
}

#define DRD_TIMEOUT 5// Number of seconds after reset during which a subseqent reset will be considered a double reset.
#define DRD_ADDRESS 0 // RTC Memory Address for the DoubleResetDetector to use
DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

unsigned long eeprom_millis;
double save_temp;

uint8_t PIN_THERMOSTAT;
uint8_t PIN_THERMOMETR;
uint8_t LED_CONFIG_PIN;
uint8_t CONFIG_PIN;
uint8_t MAX_DS18B20;
uint8_t PIN_BUTTON_AUTO;
uint8_t PIN_BUTTON_MANUAL;

int nr_button = 0;
int nr_relay = 0;
int invert = 0;
int nr_ds18b20 = 0;
int nr_dht = 0;

double temp_html;
double humidity_html;

const char* Config_Wifi_name = CONFIG_WIFI_LOGIN;
const char* Config_Wifi_pass = CONFIG_WIFI_PASSWORD;

unsigned long check_delay_WiFi = 50000;
unsigned long wait_for_WiFi;

//CONFIG
int config_state = HIGH;
int last_config_state = HIGH;
unsigned long time_last_config_change;
long config_delay = 5000;

const char* www_username;
const char* www_password;
const char* update_path = UPDATE_PATH;

WiFiClient client;
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
ETSTimer led_timer;

_relay_button_channel relay_button_channel[MAX_RELAY];

// Setup a DHT instance
DHT *dht_sensor;
int dht_channel[MAX_DHT];
int channelNumberDHT = 0;

// Setup a DS18B20 instance
//OneWire ds18x20[MAX_DS18B20_ARR] = 0;
OneWire ds18x20[MAX_DS18B20_ARR] = 0;
//const int oneWireCount = sizeof(ds18x20) / sizeof(OneWire);
DallasTemperature sensor[MAX_DS18B20_ARR];
_ds18b20_channel_t ds18b20_channel[MAX_DS18B20_ARR];
int ds18b20_channel_first = 0;

//SUPLA ****************************************************************************************************

char Supla_server[MAX_SUPLA_SERVER];
char Location_id[MAX_SUPLA_ID];
char Location_Pass[MAX_SUPLA_PASS];
//*********************************************************************************************************
void setup() {
  Serial.begin(74880);
  EEPROM.begin(EEPROM_SIZE);

  if ('2' == char(EEPROM.read(EEPROM_SIZE - 1))) {
    czyszczenieEeprom();
    first_start();
  } else if ('1' != char(EEPROM.read(EEPROM_SIZE - 1))) {
    czyszczenieEepromAll();
    first_start();
    save_guid();
  }

  SuplaDevice.setStatusFuncImpl(&status_func);
  SuplaDevice.setDigitalReadFuncImpl(&supla_DigitalRead);
  SuplaDevice.setDigitalWriteFuncImpl(&supla_DigitalWrite);
  SuplaDevice.setTimerFuncImpl(&supla_timer);

  thermostat_start();
  supla_board_configuration();
  supla_ds18b20_start();
  supla_dht_start();
  supla_start();

  if (String(read_wifi_ssid().c_str()) == 0
      || String(read_wifi_pass().c_str()) == 0
      || String(read_login().c_str()) == 0
      || String(read_login_pass().c_str()) == 0
      || String(read_supla_server().c_str()) == 0
      || String(read_supla_id().c_str()) == 0
      || String(read_supla_pass().c_str()) == 0
      || read_gpio(3) == -1
     ) {

    gui_color = GUI_GREEN;
    Modul_tryb_konfiguracji = 2;
    Tryb_konfiguracji();
  }

  if (drd.detectDoubleReset()) {
    drd.stop();
    gui_color = GUI_GREEN;
    Modul_tryb_konfiguracji = 2;
    Tryb_konfiguracji();
  }
  else gui_color = GUI_BLUE;

  //Pokaz_zawartosc_eeprom();

  Serial.println();
  Serial.println("Uruchamianie serwera...");

  createWebServer();
}

//*********************************************************************************************************

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi_up();
  } else {
    httpServer.handleClient();
  }

  SuplaDevice.iterate();
  drd.loop();

}
//*********************************************************************************************************

// Supla.org ethernet layer
int supla_arduino_tcp_read(void *buf, int count) {
  _supla_int_t size = client.available();

  if ( size > 0 ) {
    if ( size > count ) size = count;
    return client.read((uint8_t *)buf, size);
  }

  return -1;
}

int supla_arduino_tcp_write(void *buf, int count) {
  return client.write((const uint8_t *)buf, count);
}

bool supla_arduino_svr_connect(const char *server, int port) {
  if (WiFi.status() == WL_CONNECTED) return client.connect(server, 2015); else return false;
}

bool supla_arduino_svr_connected(void) {
  return client.connected();
}

void supla_arduino_svr_disconnect(void) {
  client.stop();
}

void supla_arduino_eth_setup(uint8_t mac[6], IPAddress *ip) {
  WiFi_up();
}

int supla_DigitalRead(int channelNumber, uint8_t pin) {
  if (pin == VIRTUAL_PIN_THERMOSTAT_AUTO) {
    return thermostat.last_state_auto;
  }

  if (pin == VIRTUAL_PIN_THERMOSTAT_MANUAL) {
    return thermostat.last_state_manual;
  }

  if (pin == VIRTUAL_PIN_SENSOR_THERMOSTAT) {

    if (thermostat.type == THERMOSTAT_PWM || thermostat.type == THERMOSTAT_PWM_HUMIDITY) {
      return relayStatus ? 1 : 0;
    } else {
      return digitalRead(PIN_THERMOSTAT) == (thermostat.invertRelay ? 1 : 0);
    }
  }

  if ( pin == VIRTUAL_PIN_SET_TEMP ) {
    return -1;
  }
  return digitalRead(pin);
}

void supla_DigitalWrite(int channelNumber, uint8_t pin, uint8_t val) {
  if ( pin == VIRTUAL_PIN_THERMOSTAT_AUTO && val != thermostat.last_state_auto) {
    Serial.println("sterowanie automatyczne");
    //SuplaDevice.channelValueChanged(channelNumber, val);
    if (val) {
      thermostat.last_state_manual = 0;
      SuplaDevice.channelValueChanged(thermostat.channelManual, 0);
    } else  {
      thermostatOFF();
    }
    digitalWrite(LED_CONFIG_PIN, val);
    thermostat.last_state_auto = val;
    return;
  }

  if ( pin == VIRTUAL_PIN_THERMOSTAT_MANUAL && val != thermostat.last_state_manual && thermostat.last_state_auto == 0) {
    Serial.println("sterowanie manualne");
    //SuplaDevice.channelValueChanged(channelNumber, val);
    if (val) thermostatON(); else thermostatOFF();

    thermostat.last_state_manual = val;
    return;
  }

  if ( pin == VIRTUAL_PIN_SET_TEMP ) {
    if (chackThermostatHumidity()) {
      val ? thermostat.humidity += 1 : thermostat.humidity -= 1;
    } else {
      val ? thermostat.temp += 0.5 : thermostat.temp -= 0.5;
    }
    valueChangeTemp();
    eeprom_millis = millis() + 10000;
    return;
  }
  digitalWrite(pin, val);
}

void supla_timer() {
  //ZAPIS TEMP DO EEPROMA*************************************************************************************
  if ( eeprom_millis < millis() ) {
    if (chackThermostatHumidity()) {
      if ( save_temp != thermostat.humidity) {
        save_thermostat_humidity(thermostat.humidity);
        Serial.println("zapisano wartość wilgotnośći");
      }
    } else if (save_temp != thermostat.temp ) {
      save_thermostat_temp(thermostat.temp);
      Serial.println(thermostat.type);
      Serial.println("zapisano temperaturę");
    }

    if (chackThermostatHumidity()) save_temp =  thermostat.humidity; else save_temp = thermostat.temp;

    valueChangeTemp();
    eeprom_millis = millis() + 10000;
  }

  configBTN();
}

SuplaDeviceCallbacks supla_arduino_get_callbacks(void) {
  SuplaDeviceCallbacks cb;

  cb.tcp_read = &supla_arduino_tcp_read;
  cb.tcp_write = &supla_arduino_tcp_write;
  cb.eth_setup = &supla_arduino_eth_setup;
  cb.svr_connected = &supla_arduino_svr_connected;
  cb.svr_connect = &supla_arduino_svr_connect;
  cb.svr_disconnect = &supla_arduino_svr_disconnect;
  cb.get_temperature = &get_temperature;
  cb.get_temperature_and_humidity = &get_temperature_and_humidity;
  cb.get_rgbw_value = NULL;
  cb.set_rgbw_value = NULL;
  cb.read_supla_relay_state = &read_supla_relay_state;
  cb.save_supla_relay_state = &save_supla_relay_state;

  return cb;
}
//*********************************************************************************************************

void createWebServer() {

  String www_username1 = String(read_login().c_str());
  String www_password1 = String(read_login_pass().c_str());

  www_password = strcpy((char*)malloc(www_password1.length() + 1), www_password1.c_str());
  www_username = strcpy((char*)malloc(www_username1.length() + 1), www_username1.c_str());

  httpServer.on("/", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    httpServer.send(200, "text/html", supla_webpage_start(0));
  });

  httpServer.on("/set0", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }

    save_wifi_ssid(httpServer.arg("wifi_ssid"));
    save_wifi_pass(httpServer.arg("wifi_pass"));
    save_login( httpServer.arg("modul_login"));
    save_login_pass(httpServer.arg("modul_pass"));
    save_supla_server(httpServer.arg("supla_server"));
    save_supla_hostname(httpServer.arg("supla_hostname"));
    save_supla_id(httpServer.arg("supla_id"));
    save_supla_pass(httpServer.arg("supla_pass"));


    if (nr_ds18b20 != 0) {
      for (int i = 0; i < MAX_DS18B20; i++) {
        if (ds18b20_channel[i].type == 1) {
          String ds = "ds18b20_id_";
          ds += i;
          String address = httpServer.arg(ds);
          if (address != NULL) save_DS18b20_address(address, i);
        }
      }
    }

    if (Modul_tryb_konfiguracji != 0 ) {
      if (nr_button > 0) {
        for (int i = 1; i <= nr_button; ++i) {
          String button = "button_set";
          button += i;
          save_supla_button_type(i, httpServer.arg(button));
        }
      }
      if (nr_relay > 0) {
        for (int i = 1; i <= nr_relay; ++i) {
          String relay = "relay_set";
          relay += i;
          save_supla_relay_flag(i, httpServer.arg(relay));
        }
      }

      if (MAX_GPIO > 0 ) {
        for (int i = 0; i < MAX_GPIO; ++i) {
          String gpio = "gpio_set";
          gpio += i;
          save_gpio(i, httpServer.arg(gpio));
        }
      }

      thermostat.invertRelay = httpServer.arg("invert_relay").toInt();
      save_invert_relay(thermostat.invertRelay);

      /* if (thermostat.typeSensor == TYPE_SENSOR_DS18B20) {
         nr_ds18b20 = 0;
         if (MAX_DS18B20 == 1) {
           add_DS18B20_Thermometer(PIN_THERMOMETR);
         } else {
           add_DS18B20Multi_Thermometer(PIN_THERMOMETR);
         }
        } else if (thermostat.typeSensor == TYPE_SENSOR_DHT) {
         nr_dht = 1;
         supla_dht_start();
        }
        }*/

      thermostat.typeSensor = httpServer.arg("sensor_type").toInt();
      save_type_sensor(thermostat.typeSensor);

      nr_ds18b20 = 0;
      nr_dht = 0;

      if (thermostat.typeSensor == TYPE_SENSOR_DS18B20) {
        int max_ds = httpServer.arg("thermostat_max_ds").toInt();
        if (max_ds != 0) {
          MAX_DS18B20 = max_ds;
          save_thermostat_max_ds(MAX_DS18B20);
        }
        nr_ds18b20 = MAX_DS18B20;
      } else if (thermostat.typeSensor == TYPE_SENSOR_DHT) {
        nr_dht = 1;
      }
      //supla_ds18b20_start();
      //supla_dht_start();
    }

    thermostat.type = httpServer.arg("thermostat_type").toInt();
    save_thermostat_type(thermostat.type);

    if (thermostat.typeSensor == TYPE_SENSOR_DS18B20) {
      thermostat.temp = httpServer.arg("thermostat_temp").toFloat();
      save_thermostat_temp(thermostat.temp);

    } else {
      thermostat.humidity = httpServer.arg("thermostat_humidity").toInt();
      save_thermostat_humidity(thermostat.humidity);;
    }

    if (thermostat.typeSensor == TYPE_SENSOR_DS18B20) {
      if (MAX_DS18B20 != 1) {
        thermostat.channelDs18b20 = httpServer.arg("thermostat_channel").toInt();
        save_thermostat_channel(thermostat.channelDs18b20);
      } else {
        thermostat.channelDs18b20 = 0;
      }
    }


    thermostat.hyst = httpServer.arg("thermostat_hist").toFloat();
    save_thermostat_hyst(thermostat.hyst);

    httpServer.send(200, "text/html", supla_webpage_start(1));
  });

  //************************************************************

  httpServer.on("/firmware_up", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    httpServer.send(200, "text/html", supla_webpage_upddate());
  });

  //****************************************************************************************************************************************
  httpServer.on("/reboot", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    httpServer.send(200, "text/html", supla_webpage_start(2));
    delay(100);
    resetESP();
  });
  httpServer.on("/setup", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    //SetupDS18B20Multi();
    if (nr_ds18b20 != 0) {
      for (int i = 0; i < MAX_DS18B20; i++) {
        String ds = "ds18b20_id_";
        ds += i;
        String address = httpServer.arg(ds);
        if (address != NULL) {
          save_DS18b20_address(address, i);
          ds18b20_channel[i].address = address;
          read_DS18b20_address(i);
        }
      }
    }

    httpServer.send(200, "text/html", supla_webpage_search(1));
  });
  httpServer.on("/search", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    httpServer.send(200, "text/html", supla_webpage_search(0));
  });
  httpServer.on("/eeprom", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    czyszczenieEeprom();
    httpServer.send(200, "text/html", supla_webpage_start(3));
  });

  httpServer.on("/konfiguracja", []() {
    if (Modul_tryb_konfiguracji == 0) {
      if (!httpServer.authenticate(www_username, www_password))
        return httpServer.requestAuthentication();
    }
    gui_color = GUI_GREEN;
    Modul_tryb_konfiguracji = 1;
    httpServer.send(200, "text/html", supla_webpage_start(4));
  });

  httpUpdater.setup(&httpServer, UPDATE_PATH, www_username, www_password);
  httpServer.begin();
}

//****************************************************************************************************************************************
void Tryb_konfiguracji() {
  supla_led_blinking(LED_CONFIG_PIN, 100);
  my_mac_adress();
  Serial.print("Tryb konfiguracji: ");
  Serial.println(Modul_tryb_konfiguracji);

  /*
    WiFi.softAPdisconnect(true);
    delay(1000);
    WiFi.disconnect(true);
    delay(1000);
    WiFi.mode(WIFI_AP_STA);
    delay(1000);
    WiFi.softAP(Config_Wifi_name, Config_Wifi_pass);
    delay(1000);
    Serial.println("Tryb AP");
  */
  Serial.print("Creating Access Point");
  Serial.print("Setting mode ... ");
  Serial.println(WiFi.mode(WIFI_AP) ? "Ready" : "Failed!");

  while (!WiFi.softAP(Config_Wifi_name, Config_Wifi_pass))
  {
    Serial.print(".");
    delay(100);
  }
  Serial.println("Network Created!");
  Serial.print("Soft-AP IP address = ");
  Serial.println(WiFi.softAPIP());

  createWebServer();
  httpServer.begin();
  Serial.println("Start Serwera");

  if (Modul_tryb_konfiguracji == 2) {
    while (1) {
      if (WiFi.status() != WL_CONNECTED) {
        WiFi_up();
      }
      SuplaDevice.iterate();
      httpServer.handleClient();
    }
  }
}

void WiFi_up() {
  if ( WiFi.status() != WL_CONNECTED
       && millis() >= wait_for_WiFi ) {

    String esid = String(read_wifi_ssid().c_str());
    String epass = String(read_wifi_pass().c_str());

    Serial.println("WiFi init ");
    if ( esid != 0 || epass != 0 ) {
      if (Modul_tryb_konfiguracji == 0) {
        Serial.println("Creating STA");
        Serial.print("Setting mode ... ");
        Serial.println(WiFi.mode(WIFI_STA) ? "Ready" : "Failed!");
        supla_led_blinking(LED_CONFIG_PIN, 500);
      }
      Serial.print("SSID: ");
      Serial.println(esid);
      Serial.print("PASSWORD: ");
      Serial.println(epass);

      WiFi.begin(esid.c_str(), epass.c_str());
    } else {
      Serial.println("Empty SSID or PASSWORD");
    }
    wait_for_WiFi = millis() + check_delay_WiFi;
  }
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case WIFI_EVENT_STAMODE_GOT_IP:
      Serial.print("localIP: ");
      Serial.println(WiFi.localIP());
      Serial.print("subnetMask: ");
      Serial.println(WiFi.subnetMask());
      Serial.print("gatewayIP: ");
      Serial.println(WiFi.gatewayIP());
      Serial.print("siła sygnału (RSSI): ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      break;
    case WIFI_EVENT_STAMODE_DISCONNECTED:
      Serial.println("WiFi lost connection");
      break;
  }
}

void first_start(void) {
  EEPROM.begin(EEPROM_SIZE);
  delay(100);
  EEPROM.write(EEPROM_SIZE - 1, '1');
  EEPROM.end();
  delay(100);
  save_login(DEFAULT_LOGIN);
  save_login_pass(DEFAULT_PASSWORD);
  save_supla_hostname(DEFAULT_HOSTNAME);
  save_gpio(0, "12");//przekaźnik
  save_gpio(1, "3");//termometr
  save_gpio(2, "13");//led
  save_gpio(3, "0");//config
  save_gpio(4, "0");//przycisk auto
  save_gpio(5, "17");//przycisk manual domyślnie na BRAK
  save_thermostat_type(0); //grzanie
  save_thermostat_max_ds(1);
  save_type_sensor(0);
  save_invert_relay(0);

  save_thermostat_humidity(50);
  save_thermostat_temp(20);
}

void supla_start() {
  client.setTimeout(500);

  read_guid();
  int Location_id = read_supla_id().toInt();
  strcpy(Supla_server, read_supla_server().c_str());
  strcpy(Location_Pass, read_supla_pass().c_str());

  read_guid();
  my_mac_adress();

  String supla_hostname = read_supla_hostname().c_str();
  supla_hostname.replace(" ", "-");
  WiFi.hostname(supla_hostname);
  WiFi.setAutoConnect(false);
 // WiFi.setPhyMode(WIFI_PHY_MODE_11B);
 // WiFi.setOutputPower(20.5);
  WiFi.onEvent(WiFiEvent);

  SuplaDevice.setName(read_supla_hostname().c_str());


  SuplaDevice.begin(GUID,              // Global Unique Identifier
                    mac,               // Ethernet MAC address
                    Supla_server,  // SUPLA server address
                    Location_id,                 // Location ID
                    Location_Pass);

}

String read_rssi(void) {
  long rssi = WiFi.RSSI();
  return String(rssi) ;
}

void get_temperature_and_humidity(int channelNumber, double * temp, double * humidity) {

  int i = channelNumber - channelNumberDHT;

  if ( i >= 0 && nr_dht != 0 && channelNumberDHT != 0) {

    *temp = dht_sensor[i].readTemperature();
    *humidity = dht_sensor[i].readHumidity();

    //  static uint8_t error;
    // Serial.print("get_temperature_and_humidity - "); Serial.print(channelNumber); Serial.print(" -- "); Serial.print(*temp); Serial.print(" -- "); Serial.println(*humidity);
    if ( isnan(*temp) || isnan(*humidity) ) {
      *temp = -275;
      *humidity = -1;
      //    error++;
    }
    //THERMOSTAT
    CheckTermostat(-1, *temp, *humidity);
    //  Serial.print("error - "); Serial.println(error);
  }
}

double get_temperature(int channelNumber, double last_val) {
  int i = channelNumber - ds18b20_channel_first;

  if ( i >= 0 ) {
    if ( ds18b20_channel[i].address == "FFFFFFFFFFFFFFFF" ) return TEMPERATURE_NOT_AVAILABLE;

    if ( (ds18b20_channel[i].lastTemperatureRequest + 10000) <  millis() && ds18b20_channel[i].iterationComplete) {
      sensor[i].requestTemperatures();

      ds18b20_channel[i].iterationComplete = false;
      ds18b20_channel[i].lastTemperatureRequest = millis();
      //Serial.print("requestTemperatures: "); Serial.println(i);
    }

    if ( ds18b20_channel[i].lastTemperatureRequest + 5000 <  millis()) {
      double t = -275;

      if (nr_ds18b20 == 1) {
        t = sensor[0].getTempCByIndex(0);
      } else {
        t = sensor[i].getTempC(ds18b20_channel[i].deviceAddress);
      }

      if (t == DEVICE_DISCONNECTED_C || t == 85.0) {
        t = TEMPERATURE_NOT_AVAILABLE;
      }

      if (t == TEMPERATURE_NOT_AVAILABLE) {
        ds18b20_channel[i].retryCounter++;
        if (ds18b20_channel[i].retryCounter > 3) {
          ds18b20_channel[i].retryCounter = 0;
        } else {
          t = ds18b20_channel[i].last_val;
        }
      } else {
        ds18b20_channel[i].retryCounter = 0;
      }
      CheckTermostat(i, t, 0);
      ds18b20_channel[i].last_val = t;
      ds18b20_channel[i].iterationComplete = true;

      //Serial.print("getTempC: "); Serial.print(i); Serial.print(" temp: "); Serial.println(t);
    }
  }
  return ds18b20_channel[i].last_val;
}

void supla_led_blinking_func(void *timer_arg) {
  int val = digitalRead(LED_CONFIG_PIN);
  digitalWrite(LED_CONFIG_PIN, val == HIGH ? 0 : 1);
}

void supla_led_blinking(int led, int time) {

  os_timer_disarm(&led_timer);
  os_timer_setfn(&led_timer, supla_led_blinking_func, NULL);
  os_timer_arm (&led_timer, time, true);

}

void supla_led_blinking_stop(void) {
  os_timer_disarm(&led_timer);
  digitalWrite(LED_CONFIG_PIN, 1);
}

void supla_led_set(int ledPin) {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, 1);
}

void supla_ds18b20_start(void) {
  if (nr_ds18b20 != 0 ) {
    Serial.print("DS18B2 init: "); Serial.println(nr_ds18b20);
    Serial.print("Parasite power is: ");
    if ( sensor[0].isParasitePowerMode() ) {
      Serial.println("ON");
    } else {
      Serial.println("OFF");
    }
    for (int i = 0; i < nr_ds18b20; i++) {
      sensor[i].setOneWire(&ds18x20[i]);
      sensor[i].begin();

      if (ds18b20_channel[i].type == 1) {
        sensor[i].setResolution(ds18b20_channel[i].deviceAddress, TEMPERATURE_PRECISION);
      } else {
        if (sensor[i].getAddress(ds18b20_channel[i].deviceAddress, 0)) sensor[i].setResolution(ds18b20_channel[i].deviceAddress, TEMPERATURE_PRECISION);
      }

      sensor[i].setWaitForConversion(false);
      sensor[i].requestTemperatures();

      ds18b20_channel[i].iterationComplete = false;
      ds18b20_channel[i].lastTemperatureRequest = -2500;
    }
  }
}

void supla_dht_start(void) {
  if (nr_dht != 0 ) {
    Serial.print("DHT init: "); Serial.println(nr_dht);
    for (int i = 0; i < nr_dht; i++) {
      dht_sensor[i].begin();
    }
  }
}

void add_Sensor(int sensor) {
  SuplaDevice.addSensorNO(sensor);
}

void add_Led_Config(int led) {
  supla_led_set(led);
}

void add_Config(int pin) {
  pinMode(pin, INPUT_PULLUP);
}

void add_Relay(int relay) {
  relay_button_channel[nr_relay].relay = relay;
  relay_button_channel[nr_relay].invert = 0;
  nr_relay++;
  //SuplaDevice.addRelay(relay);
  SuplaDevice.addRelayButton(relay, -1, 0, read_supla_relay_flag(nr_relay));
}

void add_Relay_Invert(int relay) {
  relay_button_channel[nr_relay].relay = relay;
  relay_button_channel[nr_relay].invert = 1;
  nr_relay++;
  //SuplaDevice.addRelay(relay, true);
  SuplaDevice.addRelayButton(relay, -1, 0, read_supla_relay_flag(nr_relay), true);
}

void add_DHT11_Thermometer(int thermpin) {
  int channel = SuplaDevice.addDHT11();

  if (nr_dht == 0) channelNumberDHT = channel;

  dht_sensor = (DHT*)realloc(dht_sensor, sizeof(DHT) * (nr_dht + 1));

  dht_sensor[nr_dht] = { thermpin, DHT11 };
  dht_channel[nr_dht] = channel;
  nr_dht++;
}

void add_DHT22_Thermometer(int thermpin) {
  int channel = SuplaDevice.addDHT22();
  if (nr_dht == 0) channelNumberDHT = channel;

  dht_sensor = (DHT*)realloc(dht_sensor, sizeof(DHT) * (nr_dht + 1));

  dht_sensor[nr_dht] = { thermpin, DHT22 };
  dht_channel[nr_dht] = channel;
  nr_dht++;
}

void add_Relay_Button(int relay, int button, int type) {
  relay_button_channel[nr_relay].relay = relay;
  relay_button_channel[nr_relay].invert = 0;
  nr_button++;
  nr_relay++;
  if (type == CHOICE_TYPE) {
    int select_button = read_supla_button_type(nr_button);
    type = select_button;
  }

  SuplaDevice.addRelayButton(relay, button, type, read_supla_relay_flag(nr_relay));
}

void add_Relay_Button_Invert(int relay, int button, int type) {
  relay_button_channel[nr_relay].relay = relay;
  relay_button_channel[nr_relay].invert = 1;
  nr_button++;
  nr_relay++;
  if (type == CHOICE_TYPE) {
    int select_button = read_supla_button_type(nr_button);
    type = select_button;
  }

  SuplaDevice.addRelayButton(relay, button, type, read_supla_relay_flag(nr_relay), true);
}

void add_DS18B20_Thermometer(int thermpin) {
  int channel = SuplaDevice.addDS18B20Thermometer();
  if (nr_ds18b20 == 0) ds18b20_channel_first = channel;

  ds18x20[nr_ds18b20] = thermpin;
  ds18b20_channel[nr_ds18b20].pin = thermpin;
  ds18b20_channel[nr_ds18b20].channel = channel;
  ds18b20_channel[nr_ds18b20].type = 0;
  //ds18b20_channel[nr_ds18b20].name = String(read_DS18b20_name(nr_ds18b20).c_str());
  ds18b20_channel[nr_ds18b20].last_val = -275;
  nr_ds18b20++;
}

void add_DS18B20Multi_Thermometer(int thermpin) {
  for (int i = 0; i < MAX_DS18B20; i++) {
    int channel = SuplaDevice.addDS18B20Thermometer();
    if (i == 0) ds18b20_channel_first = channel;

    ds18x20[nr_ds18b20] = thermpin;
    ds18b20_channel[nr_ds18b20].pin = thermpin;
    ds18b20_channel[nr_ds18b20].channel = channel;
    ds18b20_channel[nr_ds18b20].type = 1;
    //ds18b20_channel[nr_ds18b20].name = String(read_DS18b20_name(nr_ds18b20).c_str());
    ds18b20_channel[nr_ds18b20].address = read_DS18b20_address(i).c_str();
    ds18b20_channel[nr_ds18b20].last_val = -275;
    nr_ds18b20++;
  }
}

//Convert device id to String
String GetAddressToString(DeviceAddress deviceAddress) {
  String str = "";
  for (uint8_t i = 0; i < 8; i++) {
    if ( deviceAddress[i] < 16 ) str += String(0, HEX);
    str += String(deviceAddress[i], HEX);
  }
  return str;
}

void SetupDS18B20Multi() {
  DeviceAddress devAddr[MAX_DS18B20_ARR];  //An array device temperature sensors
  DeviceAddress tempSensor;

  //int numberOfDevices; //Number of temperature devices found
  //numberOfDevices = sensor[0].getDeviceCount();
  // Loop through each device, print out address
  for (int i = 0; i < MAX_DS18B20; i++) {

    // Search the wire for address
    if ( sensor[i].getAddress(devAddr[i], i) ) {
      Serial.print("Found device ");
      Serial.println(i, DEC);
      Serial.println("with address: " + GetAddressToString(devAddr[i]));
      Serial.println();
      save_DS18b20_address(GetAddressToString(devAddr[i]), i);
      ds18b20_channel[i].address = read_DS18b20_address(i);
    } else {
      Serial.print("Not Found device ");
      Serial.println(i, DEC);
      // save_DS18b20_address("", i);
    }
    sensor[i].requestTemperatures();
    //Get resolution of DS18b20
    Serial.print("Resolution: ");
    Serial.print(sensor[i].getResolution(devAddr[i]));
    Serial.println();

    //Read temperature from DS18b20
    float tempC = sensor[i].getTempC(devAddr[i]);
    Serial.print("Temp C: ");
    Serial.println(tempC);
  }
}

void resetESP() {
  WiFi.forceSleepBegin();
  wdt_reset();
  ESP.restart();
  while (1)wdt_reset();
}

void configBTN() {
  //CONFIG ****************************************************************************************************
  int config_read = digitalRead(CONFIG_PIN);
  if (config_read != last_config_state) {
    time_last_config_change = millis();
  }
  if ((millis() - time_last_config_change) > config_delay) {
    if (config_read != config_state) {
      Serial.println("Triger sate changed");
      config_state = config_read;
      if (config_state == LOW && Modul_tryb_konfiguracji != 1) {
        gui_color = GUI_GREEN;
        Modul_tryb_konfiguracji = 1;
        Tryb_konfiguracji();
        //client.stop();
      } else if (config_state == LOW && Modul_tryb_konfiguracji == 1) {
        resetESP();
      }
    }
  }
  last_config_state = config_read;
}
