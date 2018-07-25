// TODO: запасное питание от кроны
// TODO: пересылка SMS
#include <string.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>

#define MODEM_RX_PIN 2
#define MODEM_TX_PIN 3
#define ALARM_ALARM_PIN 4
#define RESET_SETTINGS_PIN 5
#define BACKUP_POWER_PIN 6
#define LED_PIN 13
#define VIN_ANALOG_PIN 0
#define VIN_R1 100000L
#define VIN_R2 10000L

#define STATUS_DISARM 0
#define STATUS_ARM 1
#define STATUS_PANIC 2

#define WITH_CONSOLE
//#undef WITH_CONSOLE

#define SETTINGS_MAGICK 0x5555

struct Settings {
  int magick;
  char clientPhone[16];
  boolean smsOnStatusChange;
};

const Settings defaultSettings PROGMEM = {
  SETTINGS_MAGICK,
  //"0956182556",
  "",
  false
};

Settings settings;

#define SETTINGS_ADDR 0

SoftwareSerial modem(MODEM_RX_PIN, MODEM_TX_PIN);
char buffer[400];
const char OK[] PROGMEM = "OK";
const char NULL_STR[] PROGMEM = "";

int alarmStatus = STATUS_DISARM;

boolean ledStatus = false;
unsigned long ledChangeTime = 0;

int alarmAlarmStatus = LOW;
unsigned long alarmAlarmChangeTime = 0;
int alarmAlarmShortImpulseCount = 0;

int backupPowerStatus = LOW;
unsigned long backupPowerChangeTime = 0;
boolean onBackupPower = false;

unsigned long modemInitTime = 0;

#ifdef WITH_CONSOLE
#define PRINT(x) Serial.print(x)
#define PRINTLN(x) Serial.println(x)
#else
#define PRINT(x) __asm__ __volatile__ ("nop\n\t")
#define PRINTLN(x) __asm__ __volatile__ ("nop\n\t")
#endif

#define streq_P(s1, s2) strcmp_P(s1, s2) == 0

void setup() {
  pinMode(ALARM_ALARM_PIN, INPUT);
  pinMode(RESET_SETTINGS_PIN, INPUT_PULLUP);
  pinMode(BACKUP_POWER_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  analogReference(DEFAULT);
  analogWrite(VIN_ANALOG_PIN, 0);

  digitalWrite(LED_PIN, LOW);
  
  #ifdef WITH_CONSOLE
  Serial.begin(19200);
  #endif
  PRINTLN(F("GSM Car Alarm"));

  PRINTLN(F("Reading settings from flash ROM..."));
  EEPROM.get(SETTINGS_ADDR, settings);
  if (settings.magick != SETTINGS_MAGICK) {
    PRINTLN(F("No stored settings found, use defaults"));
    memcpy_P(&settings, &defaultSettings, sizeof(settings));
  } else {
    PRINTLN(F("Use stored settings in flash ROM"));
  }
  PRINT(F("Client phone number: "));
  PRINTLN(settings.clientPhone);

  delay(2000);
  modemInit();
}

void loop() {
  pinControl();
  #ifdef WITH_CONSOLE
  consoleControl();
  #endif
  ledControl();
  modemControl();
}

void pinControl() {
  unsigned long currentTime = millis();
  int newAlarmAlarmStatus,
      newBackupPowerStatus,
      i;
  
  if ((newAlarmAlarmStatus = digitalRead(ALARM_ALARM_PIN)) != alarmAlarmStatus) {
    if (newAlarmAlarmStatus == LOW) {
      if (currentTime - alarmAlarmChangeTime <= 500) {
        alarmAlarmShortImpulseCount += 1;
      }
    }
    alarmAlarmChangeTime = currentTime;
    alarmAlarmStatus = newAlarmAlarmStatus;
  } else if (currentTime - alarmAlarmChangeTime > 500) {
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

  if ((newBackupPowerStatus = digitalRead(BACKUP_POWER_PIN)) != backupPowerStatus) {
    backupPowerChangeTime = currentTime;
    backupPowerStatus = newBackupPowerStatus;
  } else if (currentTime - backupPowerChangeTime > 500 &&
             onBackupPower != (backupPowerStatus == LOW)) {
    onBackupPower = backupPowerStatus == LOW;
    if (onBackupPower) {
      PRINTLN(F("On backup power"));
      sendSms_P(PSTR("No standard power detected, using backup power supply"));
      call();
    } else {
      PRINTLN(F("On standard power"));
      sendSms_P(PSTR("Standard power supply was restored"));
    }
  }

  for (i = 20; digitalRead(RESET_SETTINGS_PIN) == LOW && i > 0; i--) delay(100);
  if (i == 0) {
    PRINTLN(F("Reseting settings to defaults..."));
    memcpy_P(&settings, &defaultSettings, sizeof(settings));
    EEPROM.put(SETTINGS_ADDR, settings);
    for (i = 5; i > 0; i--) {
      digitalWrite(LED_PIN, HIGH);
      delay(500);
      digitalWrite(LED_PIN, LOW);
    }
    PRINTLN(F("done"));
  }
}

#ifdef WITH_CONSOLE
void consoleControl() {
  char input[50];
  int len;
  unsigned int vin;
  
  if (!Serial.available()) return;
  
  len = Serial.readBytes(input, sizeof(input) / sizeof(char) - 1);
  input[len] = '\0';
  input[strcspn(input, "\r\n")] = '\0';
  
  if (streq_P(input, PSTR("status"))) PRINTLN(getStatusText(buffer));
  if (streq_P(input, PSTR("alarm status"))) showAlarmStatus();
  if (streq_P(input, PSTR("alarm status disarm"))) setAlarmStatus(STATUS_DISARM);
  if (streq_P(input, PSTR("alarm status arm"))) setAlarmStatus(STATUS_ARM);
  if (streq_P(input, PSTR("alarm status panic"))) setAlarmStatus(STATUS_PANIC);
  if (streq_P(input, PSTR("modem status"))) showModemStatus();
  if (streq_P(input, PSTR("modem init"))) modemInit();
  if (streq_P(input, PSTR("modem reg"))) showModemReg();
  if (streq_P(input, PSTR("modem level"))) showModemLevel();
  if (streq_P(input, PSTR("modem hangup"))) modemHangup();
  if (streq_P(input, PSTR("modem shutdown"))) modemShutdown();
  if (streq_P(input, PSTR("sms on"))) {
    settings.smsOnStatusChange = true;
    EEPROM.put(SETTINGS_ADDR, settings);
  }
  if (streq_P(input, PSTR("sms off"))) {
    settings.smsOnStatusChange = false;
    EEPROM.put(SETTINGS_ADDR, settings);
  }
  if (streq_P(input, PSTR("vin"))) showVinput();
  if (streq_P(input, PSTR("test call"))) call();
  if (streq_P(input, PSTR("test sms"))) sendSms_P(PSTR("test SMS"));
}
#endif

char *modemReadData(char *buf, int maxLen) {
  int len;
  for (int c = 0; (len = modem.readBytes(buf, maxLen)) == 0 && c < 20; c++);
  buf[len] = '\0';
  return buf;
}

boolean modemCheckResponse(const char *command, const char *expect) {
  modemReadData(buffer, sizeof(buffer) / sizeof(char) - 1);
  if (expect[0] != '\0' && !strstr(buffer, expect)) {
    PRINT(command);
    PRINTLN(F(" failed!"));
    PRINTLN(F("---"));
    PRINTLN(buffer);
    PRINTLN(F("---"));
    return false;
  }
  return true;
}

boolean modemSendCommand(const char *command, const char *expect) {
  modem.println(command);
  if (expect == NULL)
    return true;
  return modemCheckResponse(command, expect);
}

boolean modemSendCommand_P(const char *command, const char *expect) {
  char cmd[150], ex[50];
  return modemSendCommand(
    strcpy_P(cmd, command),
    (expect != NULL) ? strcpy_P(ex, expect) : NULL);
}

void modemInit() {
  PRINTLN(F("Initializing modem..."));
  modem.begin(19200);
  modem.setTimeout(500);
  modemSendCommand_P(PSTR("AT"), OK); // let modem auto detect baud rate
  modemSendCommand_P(PSTR("ATH"), OK); // hanup
  modemSendCommand_P(PSTR("ATE0"), OK); // echo off
  modemSendCommand_P(PSTR("AT+CLIP=1"), OK); // enable +CLIP notification.
  modemSendCommand_P(PSTR("AT+CMGF=1"), OK); // select SMS message format to text
  modemSendCommand_P(PSTR("AT+CSCS=\"GSM\""), OK); // select charset to GSM (7bit)
  modemSendCommand_P(PSTR("AT+CMGD=1,4"), OK); // delete all SMS messages
  modemSendCommand_P(PSTR("AT+CNMI=2,1"), OK); // new SMS message indication
  //modemSendCommand_P(PSTR("AT+ENPWRSAVE=1"), OK); // power saving mode for M590
  PRINTLN(F("done"));
  modem.setTimeout(1000);
  modemInitTime = millis();
}

void modemControl() {
  char buf[200], cmd[15], ex[5], *c, *head, *phone, *body;
  int msgN = 0;
  boolean isAuthorized;

  if (!modem.available()) {
    if ((millis() - modemInitTime) > (12L * 3600L * 1000L))
      modemInit();
    return;
  }
  
  modemReadData(buffer, sizeof(buffer)/ sizeof(char) - 1);
  
  if (strstr_P(buffer, PSTR("RING"))) {
    PRINTLN(F("Ring detected"));
    isAuthorized = strstr(buffer, settings.clientPhone) != NULL;
    modemSendCommand_P(PSTR("ATH"), OK);
    if (isAuthorized) sendAlarmStatus();
  } else if (strstr(buffer, "+CMTI:")) {
    PRINTLN(F("New SMS arrived"));

    for (c = strstr(buffer, ","); c != NULL && *c != '\0' && !isDigit(*c); c++);
    for (; c != NULL && *c != '\0' && isDigit(*c); c++)
      msgN = msgN * 10 + ((int)*c - '0');
    if (msgN > 0) {
      modemSendCommand_P(PSTR("AT+CNMI=2,0"), OK);

      PRINT(F("Reading SMS #"));
      PRINTLN(msgN);
      sprintf_P(cmd, PSTR("AT+CMGR=%d"), msgN);
      modemSendCommand(cmd, strcpy_P(ex, NULL_STR));

      head = body = NULL;
      for (c = strchr(buffer, '\0') - 1; *c <= ' '; c--) *c = '\0';
      if (strcmp_P(c - 1, OK) == 0) {
        for (head = buffer; *head <= ' '; head++);
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
            if (c != NULL && (c -= 2) >= body && streq_P(c, OK))
              for (*c = '\0'; *c <= ' '; c--) *c = '\0';
          } else {
            PRINT(F("Unauthorized message: "));
            PRINTLN(buffer);
          }
        } else {
          PRINT(F("Read message failure: No header found"));
          PRINTLN(buffer);
        }
      } else {
        PRINT(F("Read message failure: No OK respose found: "));
        PRINTLN(buffer);
      }

      if (body != NULL) {
        for (c = body; *c != '\0'; c++)
          if (*c >= 'A' && *c <= 'Z') *c -= 'A' - 'a';

        PRINT(F("Received command: "));
        PRINTLN(body);

        if (isAuthorized) {
          if (streq_P(body, PSTR("sms on"))) {
            settings.smsOnStatusChange = true;
            EEPROM.put(SETTINGS_ADDR, settings);
          } else if (streq_P(body, PSTR("sms off"))) {
            settings.smsOnStatusChange = false;
            EEPROM.put(SETTINGS_ADDR, settings);
          }
          PRINTLN(F("Sending SMS"));
          sendSms(getStatusText(buf));
        }
        if (streq_P(body, PSTR("remember me")) && phone != NULL) {
          PRINT(F("Changing client phone number to: "));
          PRINTLN(phone);
          strcpy(settings.clientPhone, phone);
          EEPROM.put(SETTINGS_ADDR, settings);
        }
      }

      PRINT(F("Deleting SMS #"));
      PRINTLN(msgN);
      sprintf_P(cmd, PSTR("AT+CMGD=%d"), msgN);
      modemSendCommand(cmd, strcpy_P(ex, OK));

      modemSendCommand_P(PSTR("AT+CNMI=2,1"), OK);
    } else {
      PRINT(F("Unable to read message number: "));
      PRINTLN(buffer);
    }
  } else {
    PRINT(F("Modem data arrived: "));
    PRINTLN(buffer);
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
  sprintf_P(
    str, PSTR(
    "Alarm status is %s\n"
    "SMS info is %s\n"
    "%s: %d.%dV\n"
    "Uptime: %d %02d:%02d"),
    ((alarmStatus == STATUS_DISARM) ?
		  "DISARM" : ((alarmStatus == STATUS_ARM) ?
                  "ARM" : "PANIC")),
		((settings.smsOnStatusChange) ? "on" : "off"),
		((onBackupPower) ? "Backup power" : "Standard power"),
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
          sendSms_P(PSTR("Alarm status is DISARM"));
          break;
        case STATUS_ARM:
          sendSms_P(PSTR("Alarm status is ARM"));
          break;
        case STATUS_PANIC:
          sendSms_P(PSTR("Alarm status is PANIC"));
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
        sendSms_P(PSTR("Alarm status changed to DISARM"));
      break;
    case STATUS_ARM:
      if (settings.smsOnStatusChange)
        sendSms_P(PSTR("Alarm status changed to ARM"));
      break;
    case STATUS_PANIC:
      if (settings.smsOnStatusChange)
        sendSms_P(PSTR("Alarm status changed to PANIC"));
      call();
      break;
  }
}

void sendSms(const char *text) {
  modemSendCommand_P(PSTR("ATH"), OK);
  delay(50);
  
  sprintf_P(buffer,
            PSTR("AT+CMGS=\"%s\"\n%s\x1a"),
            settings.clientPhone,
            text);
  modem.print(buffer);
  modemReadData(buffer, sizeof(buffer)/ sizeof(char) - 1);

  PRINT(F("Send SMS: "));
  PRINTLN(buffer);
}

void sendSms_P(const char *text) {
  char buf[200];
  sendSms(strcpy_P(buf, text));
}

void call() {
  char cmd[30], ex[5];
  
  sprintf_P(cmd, PSTR("ATD%s;"),
            settings.clientPhone);
  modemSendCommand(cmd, strcpy_P(ex, NULL_STR));

  PRINT(F("Call: "));
  PRINTLN(buffer);
}

void showModemStatus() {
  modemSendCommand_P(PSTR("AT+CPAS"), NULL_STR);
  PRINT(F("Modem status: "));
  PRINTLN(buffer);
}

void showModemReg() {
  modemSendCommand_P(PSTR("AT+CREG?"), NULL_STR);
  PRINT(F("Modem registration: "));
  PRINTLN(buffer);
}

void showModemLevel() {
  modemSendCommand_P(PSTR("AT+CSQ"), NULL_STR);
  PRINT(F("Modem signal level: "));
  PRINTLN(buffer);
}

void modemShutdown() {
  modemSendCommand_P(PSTR("AT+CPWROFF"), NULL_STR);
  PRINT(F("Modem shutdown: "));
  PRINTLN(buffer);
}

void modemHangup() {
  modemSendCommand_P(PSTR("ATH"), NULL_STR);
  PRINT(F("Modem hangup: "));
  PRINTLN(buffer);
}

void showVinput() {
  char buf[20];
  unsigned int vin = readVinput() + 50;
  sprintf_P(buf, PSTR("V input: %d.%dV"),
            vin / 1000, vin % 1000 / 100);
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
