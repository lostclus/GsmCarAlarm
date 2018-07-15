// TODO: запасное питание от кроны
// TODO: пересылка SMS
#include <EEPROM.h>
#include <SoftwareSerial.h>

#define MODEM_RX_PIN 2
#define MODEM_TX_PIN 3
#define ALARM_ALARM_PIN 4
#define RESET_SETTINGS_PIN 5
#define LED_PIN 13
#define VIN_ANALOG_PIN 0
#define VIN_R1 100000L
#define VIN_R2 10000L

#define STATUS_DISARM 0
#define STATUS_ARM 1
#define STATUS_PANIC 2

#define CONSOLE
//#undef CONSOLE

struct Settings {
  int magick;
  char clientPhone[16];
  boolean smsOnStatusChange;
};

Settings settings;
Settings defaultSettings = {
  0x5555,
  //"0956182556",
  "",
  false
};

#define SETTINGS_ADDR 0

SoftwareSerial modem(MODEM_RX_PIN, MODEM_TX_PIN);
char modemDataBuf[300];
char smsBuf[160];
int alarmStatus = STATUS_DISARM;

boolean ledStatus = false;
unsigned long ledChangeTime = 0;
int alarmAlarmStatus = LOW;
unsigned long alarmAlarmChangeTime = 0;
int alarmAlarmShortImpulseCount = 0;
unsigned long modemInitTime = 0;

#ifdef CONSOLE
#define PRINT(x) Serial.print(x)
#define PRINTLN(x) Serial.println(x)
#else
#define PRINT(x) {}
#define PRINTLN(x) {}
#endif

#define streq(s1, s2) strcmp(s1, s2) == 0

void setup() {
  pinMode(ALARM_ALARM_PIN, INPUT);
  pinMode(RESET_SETTINGS_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  analogReference(DEFAULT);
  analogWrite(VIN_ANALOG_PIN, 0);

  digitalWrite(LED_PIN, LOW);
  
  #ifdef CONSOLE
  Serial.begin(19200);
  #endif
  PRINTLN(F("GSM Car Alarm"));

  PRINTLN("Reading settings from EEPROM...");
  EEPROM.get(SETTINGS_ADDR, settings);
  if (settings.magick != defaultSettings.magick) {
    PRINTLN("No stored settings found, use defaults");
    memcpy(&settings, &defaultSettings, sizeof(settings));
  } else {
    PRINTLN("Use stored settings in EEPROM");
  }
  PRINT("Client phone number: ");
  PRINTLN(settings.clientPhone);

  delay(2000);
  modemInit();
}

void loop() {
  pinControl();
  #ifdef CONSOLE
  consoleControl();
  #endif
  ledControl();
  modemControl();
}

void pinControl() {
  unsigned long currentTime = millis();
  int newAlarmAlarmStatus = digitalRead(ALARM_ALARM_PIN);
  
  if (newAlarmAlarmStatus != alarmAlarmStatus) {
    if (newAlarmAlarmStatus == LOW) {
      if (currentTime - alarmAlarmChangeTime <= 500) {
        alarmAlarmShortImpulseCount += 1;
      }
    }
    alarmAlarmChangeTime = currentTime;
    alarmAlarmStatus = newAlarmAlarmStatus;
  } else {
    if (currentTime - alarmAlarmChangeTime > 500) {
      if (alarmAlarmStatus == LOW) {
        if (alarmAlarmShortImpulseCount == 1 || alarmAlarmShortImpulseCount == 3)
          setAlarmStatus(STATUS_ARM);
        else if (alarmAlarmShortImpulseCount == 2 || alarmAlarmShortImpulseCount == 4)
          setAlarmStatus(STATUS_DISARM);
      } else {
        if (currentTime - alarmAlarmChangeTime > 2000)
          setAlarmStatus(STATUS_PANIC);
      }
      alarmAlarmShortImpulseCount = 0;
    }
  }

  if (digitalRead(RESET_SETTINGS_PIN) == LOW) {
    PRINTLN("Reset settings to defaults...");
    memcpy(&settings, &defaultSettings, sizeof(settings));
    EEPROM.put(SETTINGS_ADDR, settings);
    delay(5000);
    PRINTLN("done");
  }
}

#ifdef CONSOLE
void consoleControl() {
  char input[50];
  int len;
  unsigned int vin;
  
  if (!Serial.available()) return;
  
  len = Serial.readBytes(input, sizeof(input) / sizeof(char) - 1);
  input[len] = '\0';
  input[strcspn(input, "\r\n")] = '\0';
  
  if (streq(input, "status")) PRINTLN(getStatusText(smsBuf));
  if (streq(input, "alarm status")) showAlarmStatus();
  if (streq(input, "alarm status disarm")) setAlarmStatus(STATUS_DISARM);
  if (streq(input, "alarm status arm")) setAlarmStatus(STATUS_ARM);
  if (streq(input, "alarm status panic")) setAlarmStatus(STATUS_PANIC);
  if (streq(input, "modem status")) showModemStatus();
  if (streq(input, "modem init")) modemInit();
  if (streq(input, "modem reg")) showModemReg();
  if (streq(input, "modem hangup")) modemHangup();
  if (streq(input, "modem shutdown")) modemShutdown();
  if (streq(input, "sms on")) {
    settings.smsOnStatusChange = true;
    EEPROM.put(SETTINGS_ADDR, settings);
  }
  if (streq(input, "sms off")) {
    settings.smsOnStatusChange = false;
    EEPROM.put(SETTINGS_ADDR, settings);
  }
  if (streq(input, "vin")) showVinput();
  if (streq(input, "test call")) call();
  if (streq(input, "test sms")) sendSms("test SMS");
}
#endif

char *modemReadData(SoftwareSerial &modem, char *buf, int maxLen) {
  int len;
  for (int c = 0; (len = modem.readBytes(buf, maxLen)) == 0 && c < 20; c++);
  buf[len] = '\0';
  return buf;
}

bool modemSendCommand(SoftwareSerial &modem,
                      const char *command,
                      const char *expect) {
  modem.println(command);
  modemReadData(modem, modemDataBuf,
                sizeof(modemDataBuf) / sizeof(char) - 1);
  if (expect != NULL && !strstr(modemDataBuf, expect)) {
    PRINT(command);
    PRINTLN(F(" failed!"));
    PRINTLN(F("---"));
    PRINTLN(modemDataBuf);
    PRINTLN(F("---"));
  }
}

void modemInit() {
  PRINTLN(F("Initializing modem..."));
  modem.begin(19200);
  modem.setTimeout(500);
  modemSendCommand(modem, "AT", "OK"); // let modem auto detect baud rate
  modemSendCommand(modem, "ATH", "OK"); // hanup
  modemSendCommand(modem, "ATE0", "OK"); // echo off
  modemSendCommand(modem, "AT+CLIP=1", "OK"); // enable +CLIP notification.
  modemSendCommand(modem, "AT+CMGF=1", "OK"); // select SMS message format to text
  modemSendCommand(modem, "AT+CSCS=\"GSM\"", "OK"); // select charset to GSM (7bit)
  modemSendCommand(modem, "AT+CMGD=1,4", "OK"); // delete all SMS messages
  modemSendCommand(modem, "AT+CNMI=2,1", "OK"); // new SMS message indication
  //modemSendCommand(modem, "AT+ENPWRSAVE=1", "OK"); // power saving mode for M590
  PRINTLN(F("done"));
  modem.setTimeout(1000);
  modemInitTime = millis();
}

void modemControl() {
  char *c, cmd[12], *head, *phone, *body;
  int msgN = 0;
  boolean isAuthorized;

  if (!modem.available()) {
    if ((millis() - modemInitTime) > (12L * 3600L * 1000L))
      modemInit();
    return;
  }
  
  modemReadData(modem, modemDataBuf,
                sizeof(modemDataBuf)/ sizeof(char) - 1);
  
  if (strstr(modemDataBuf, "RING")) {
    PRINTLN(F("Ring detected"));
    isAuthorized = strstr(modemDataBuf, settings.clientPhone) != NULL;
    modemSendCommand(modem, "ATH", "OK");
    if (isAuthorized) sendAlarmStatus();
  } else if (strstr(modemDataBuf, "+CMTI:")) {
    PRINTLN(F("New SMS arrived"));

    for (c = strstr(modemDataBuf, ","); c != NULL && *c != '\0' && !isDigit(*c); c++);
    for (; c != NULL && *c != '\0' && isDigit(*c); c++)
      msgN = msgN * 10 + ((int)*c - '0');
    if (msgN > 0) {
      modemSendCommand(modem, "AT+CNMI=2,0", "OK");

      PRINT(F("Reading SMS #"));
      PRINTLN(msgN);
      sprintf(cmd, "AT+CMGR=%d", msgN);
      modem.println(cmd);
      modemReadData(modem, modemDataBuf,
                    sizeof(modemDataBuf) / sizeof(char) - 1);

      head = body = NULL;
      for (c = strchr(modemDataBuf, '\0') - 1; *c <= ' '; c--) *c = '\0';
      if (strcmp(c - 1, "OK") == 0) {
        for (head = modemDataBuf; *head <= ' '; head++);
        if ((c = strpbrk(head, "\r\n")) != NULL) {
          *c = '\0';
          body = c + 1;

          phone = NULL;
          for (c = strtok(head, "\""); c != NULL; c = strtok(NULL, "\"")) {
            if (strspn(c, "1234567890+") == strlen(c)) {
              phone = c;
              break;
            }
          }
          if (phone != NULL) {
            PRINT(F("Senders phone number: "));
            PRINTLN(phone);
          } else {
            PRINTLN(F("Unable to read senders phone number"));
          }

          isAuthorized = (settings.clientPhone[0] != '\0' &&
                          phone != NULL &&
                          strcmp(phone, settings.clientPhone) == 0);
          if (isAuthorized || settings.clientPhone[0] == '\0') {
            for (; *body != '\0' && *body <= ' '; body++);
            for (c = strchr(body, '\0') - 1; *c <= ' '; c--) *c = '\0';
            c = strchr(body, '\0');
            if (c != NULL && (c -= 2) >= body && streq(c, "OK"))
              for (*c = '\0'; *c <= ' '; c--) *c = '\0';
          } else {
            PRINT(F("Unauthorized message: "));
            PRINTLN(modemDataBuf);
          }
        } else {
          PRINT(F("Read message failure: "));
          PRINTLN(modemDataBuf);
        }
      } else {
        PRINT(F("Read message failure: "));
        PRINTLN(modemDataBuf);
      }

      if (body != NULL) {
        for (c = body; *c != '\0'; c++)
          if (*c >= 'A' && *c <= 'Z') *c -= 'A' - 'a';

        PRINT(F("Received command: "));
        PRINTLN(body);

        if (isAuthorized) {
          if (streq(body, "sms on")) {
            settings.smsOnStatusChange = true;
            EEPROM.put(SETTINGS_ADDR, settings);
          } else if (streq(body, "sms off")) {
            settings.smsOnStatusChange = false;
            EEPROM.put(SETTINGS_ADDR, settings);
          }
          PRINTLN(F("Sending SMS"));
          sendSms(getStatusText(smsBuf));
        }
        if (streq(body, "remember me") && phone != NULL) {
          PRINT(F("Changing client phone number to: "));
          PRINTLN(phone);
          strcpy(settings.clientPhone, phone);
          EEPROM.put(SETTINGS_ADDR, settings);
        }
      }

      PRINT(F("Deleting SMS #"));
      PRINTLN(msgN);
      sprintf(cmd, "AT+CMGD=%d", msgN);
      modemSendCommand(modem, cmd, "OK");

      modemSendCommand(modem, "AT+CNMI=2,1", "OK");
    } else {
      PRINT(F("Unable to read message number: "));
      PRINTLN(modemDataBuf);
    }
  } else {
    PRINT(F("Modem data arrived: "));
    PRINTLN(modemDataBuf);
  }
}

void ledControl() {
  unsigned long flashInterval;
  unsigned long currentTime;
  
  switch (alarmStatus) {
    case STATUS_DISARM:
      flashInterval = 0;
      break;
    case STATUS_ARM:
      flashInterval = 1000;
      break;
    case STATUS_PANIC:
      flashInterval = 100;
      break;
  }
  
  currentTime = millis();
  if (flashInterval == 0) {
    if (ledStatus) {
      ledStatus = false;
      digitalWrite(LED_PIN, LOW);
      ledChangeTime = currentTime;
    }
  } else if (currentTime - ledChangeTime >= flashInterval) {
    ledStatus = not ledStatus;
    digitalWrite(LED_PIN, (ledStatus ? HIGH : LOW));
    ledChangeTime = currentTime;
  }
}

char *getStatusText(char *str) {
  unsigned int vin;
  unsigned long uptime;

  vin = readVinput() + 50;
  uptime = millis() / 1000;
  sprintf(str, "Alarm status is %s\n"
               "SMS info is %s\n"
               "Vin: %d.%dV\n"
               "Uptime: %d %02d:%02d",
         ((alarmStatus == STATUS_DISARM) ?
		       "DISARM" : ((alarmStatus == STATUS_ARM) ?
                       "ARM" : "PANIC")),
		     ((settings.smsOnStatusChange) ? "on" : "off"),
		     vin / 1000, vin % 1000 / 100,
         int(uptime / (24 * 3600)),
         int((uptime % (24 * 3600)) / 3600),
         int((uptime % 3600) / 60));
  return str;
}

void showAlarmStatus() {
  switch (alarmStatus) {
    case STATUS_DISARM:
      PRINTLN(F("Alarm status: disarm"));
      break;
    case STATUS_ARM:
      PRINTLN(F("Alarm status: arm"));
      break;
    case STATUS_PANIC:
      PRINTLN(F("Alarm status: panic"));
      break;
  }
}

void sendAlarmStatus() {
  switch (alarmStatus) {
        case STATUS_DISARM:
          sendSms("Alarm status is DISARM");
          break;
        case STATUS_ARM:
          sendSms("Alarm status is ARM");
          break;
        case STATUS_PANIC:
          sendSms("Alarm status is PANIC");
          break;
  }
}

void setAlarmStatus(int newStatus) {
  if (newStatus == alarmStatus) return;
  
  alarmStatus = newStatus;
  showAlarmStatus();
  
  switch (alarmStatus) {
    case STATUS_DISARM:
      if (settings.smsOnStatusChange)
        sendSms("Alarm status changed to DISARM");
      break;
    case STATUS_ARM:
      if (settings.smsOnStatusChange)
        sendSms("Alarm status changed to ARM");
      break;
    case STATUS_PANIC:
      if (settings.smsOnStatusChange)
        sendSms("Alarm status changed to PANIC");
      call();
      break;
  }
}

void sendSms(const char *text) {
  char cmd[30];

  modemSendCommand(modem, "ATH", "OK");
  delay(50);
  
  sprintf(cmd, "AT+CMGS=\"%s\"", settings.clientPhone);
  modemSendCommand(modem, cmd, NULL);
  modem.print(text);
  modem.print((char)26);
  modemReadData(modem, modemDataBuf,
                sizeof(modemDataBuf)/ sizeof(char) - 1);

  PRINT(F("Send SMS: "));
  PRINTLN(modemDataBuf);
}

void call() {
  char cmd[30];
  
  sprintf(cmd, "ATD%s;", settings.clientPhone);
  modemSendCommand(modem, cmd, NULL);

  PRINT(F("Call: "));
  PRINTLN(modemDataBuf);
}

void showModemStatus() {
  modemSendCommand(modem, "AT+CPAS", NULL);
  PRINT(F("Modem status: "));
  PRINTLN(modemDataBuf);
}

void showModemReg() {
  modemSendCommand(modem, "AT+CREG?", NULL);
  PRINT(F("Modem registration: "));
  PRINTLN(modemDataBuf);
}

void modemShutdown() {
  modemSendCommand(modem, "AT+CPWROFF", NULL);
  PRINT(F("Modem shutdown: "));
  PRINTLN(modemDataBuf);
}

void modemHangup() {
  modemSendCommand(modem, "ATH", NULL);
  PRINT(F("Modem hangup: "));
  PRINTLN(modemDataBuf);
}

void showVinput() {
  char buf[20];
  unsigned int vin = readVinput() + 50;
  sprintf(buf, "V input: %d.%dV", vin / 1000, vin % 1000 / 100);
  PRINTLN(buf);
}

unsigned int readVinput() {
  byte i, count = 5;
  unsigned long vcc = 0,
                vpin = 0;

  for (i = 0; i < count; i++) {
    // Read 1.1V reference against AVcc
    // set the reference to Vcc and the measurement to the internal 1.1V reference
    #if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
        ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
    #elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
        ADMUX = _BV(MUX5) | _BV(MUX0);
    #elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
        ADMUX = _BV(MUX3) | _BV(MUX2);
    #else
        // works on an Arduino 168 or 328
        ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
    #endif

    delay(3); // Wait for Vref to settle
    ADCSRA |= _BV(ADSC); // Start conversion
    while (bit_is_set(ADCSRA,ADSC)); // measuring

    uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH
    uint8_t high = ADCH; // unlocks both

    // 1.1 * 1023 * 1000 = 1125300
    vcc += 1125300L / ((unsigned long)((high<<8) | low));
    vpin += analogRead(VIN_ANALOG_PIN);

    delay(10);
  }

  vcc = vcc / count;
  vpin = vpin / count;

  // return (vpin * vcc) / 1024 / (VIN_R2 / (VIN_R1 + VIN_R2));
  return (vpin * vcc) / 1024 * (1000L / (VIN_R2 * 1000L / (VIN_R1 + VIN_R2)));
}

// vim:et:ci:pi:sts=0:sw=2:ts=2:ai
