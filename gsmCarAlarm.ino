// TODO: запасное питание от кроны
// TODO: пересылка SMS
#include <SoftwareSerial.h>

#define LED_PIN 13
#define ALARM_ALARM_PIN 4
#define VIN_ANALOG_PIN 0
#define VIN_R1 100000L
#define VIN_R2 10000L

#define STATUS_DISARM 0
#define STATUS_ARM 1
#define STATUS_PANIC 2

const char CLIENT_PHONE_NUMBER[] = "0956182556";

SoftwareSerial modem(2, 3);
char modemData[400];
int alarmStatus = STATUS_DISARM;
boolean smsOnStatusChange = false;

boolean ledStatus = false;
unsigned long ledChangeTime = 0;
int alarmAlarmStatus = LOW;
unsigned long alarmAlarmChangeTime = 0;
int alarmAlarmShortImpulseCount = 0;
unsigned long modemInitTime = 0;

#define streq(s1, s2) strcmp(s1, s2) == 0


void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(ALARM_ALARM_PIN, INPUT);
  analogReference(DEFAULT);
  analogWrite(VIN_ANALOG_PIN, 0);
  
  delay(2000);
  digitalWrite(LED_PIN, LOW);
  
  Serial.begin(9600);
  Serial.println(F("GSM car alarm module"));

  modemInit();
}

void loop() {
  pinControl();
  monitorControl();
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
}

void monitorControl() {
  char input[100], buf[20];
  int len;
  unsigned int vin;
  
  if (!Serial.available()) return;
  
  len = Serial.readBytes(input, sizeof(input) / sizeof(char) - 1);
  input[len] = '\0';
  
  if (streq(input, "alarm status")) showAlarmStatus();
  if (streq(input, "alarm status disarm")) setAlarmStatus(STATUS_DISARM);
  if (streq(input, "alarm status arm")) setAlarmStatus(STATUS_ARM);
  if (streq(input, "alarm status panic")) setAlarmStatus(STATUS_PANIC);
  if (streq(input, "modem status")) showModemStatus();
  if (streq(input, "modem init")) modemInit();
  if (streq(input, "modem reg")) showModemReg();
  if (streq(input, "modem hangup")) modemHangup();
  if (streq(input, "modem shutdown")) modemShutdown();
  if (streq(input, "sms on")) smsOnStatusChange = true;
  if (streq(input, "sms off")) smsOnStatusChange = false;
  if (streq(input, "vin")) showVinput();
  if (streq(input, "test call")) call();
  if (streq(input, "test sms")) sendSms("test SMS");
}

char *modemReadData(SoftwareSerial &modem, char *buf, int maxLen) {
  int len;
  for (int c = 0; (len = modem.readBytes(buf, maxLen)) == 0 && c < 20; c++);
  buf[len] = '\0';
  return buf;
}

bool modemSendCommand(SoftwareSerial &modem, const char *command, const char *expect) {
  modem.println(command);
  modemReadData(modem, modemData, sizeof(modemData) / sizeof(char) - 1);
  if (expect != NULL && !strstr(modemData, expect)) {
    Serial.print(command);
    Serial.println(F(" failed!"));
    Serial.println(F("---"));
    Serial.println(modemData);
    Serial.println(F("---"));
  }
}

void modemInit() {
  Serial.println(F("Initializing modem..."));
  modem.begin(9600);
  modem.setTimeout(500);
  modemSendCommand(modem, "ATH", "OK");
  modemSendCommand(modem, "ATE0", "OK");
  modemSendCommand(modem, "AT+CLIP=1", "OK");
  modemSendCommand(modem, "AT+CMGF=1", "OK");
  modemSendCommand(modem, "AT+CSCS=\"GSM\"", "OK");
  modemSendCommand(modem, "AT+CMGD=1,4", "OK");
  modemSendCommand(modem, "AT+CNMI=2,1", "OK");
  modemSendCommand(modem, "AT+ENPWRSAVE=1", "OK");
  Serial.println(F("done"));
  modem.setTimeout(1000);
  modemInitTime = millis();
}

void modemControl() {
  char *c, cmd[12], *head, *body, sms[100];
  int msgN = 0;
  boolean isAuthorized;
  unsigned int vin;

  if (!modem.available()) {
    if ((millis() - modemInitTime) > (12L * 3600L * 1000L))
      modemInit();
    return;
  }
  
  modemReadData(modem, modemData, sizeof(modemData)/ sizeof(char) - 1);
  
  if (strstr(modemData, "RING")) {
    Serial.println(F("Ring detected"));
    isAuthorized = strstr(modemData, CLIENT_PHONE_NUMBER) != NULL;
    modemSendCommand(modem, "ATH", "OK");
    if (isAuthorized) sendAlarmStatus();
  } else if (strstr(modemData, "+CMTI:")) {
    Serial.println(F("New SMS arrived"));

    for (c = strstr(modemData, ","); c != NULL && *c != '\0' && !isDigit(*c); c++);
    for (; c != NULL && *c != '\0' && isDigit(*c); c++)
      msgN = msgN * 10 + ((int)*c - '0');
    if (msgN > 0) {
      modemSendCommand(modem, "AT+CNMI=2,0", "OK");

      Serial.print(F("Reading SMS #"));
      Serial.println(msgN);
      sprintf(cmd, "AT+CMGR=%d", msgN);
      modem.println(cmd);
      modemReadData(modem, modemData, sizeof(modemData) / sizeof(char) - 1);

      head = body = NULL;
      for (c = strchr(modemData, '\0') - 1; *c <= ' '; c--) *c = '\0';
      if (strcmp(c - 1, "OK") == 0) {
        for (head = modemData; *head <= ' '; head++);
        if ((c = strpbrk(head, "\r\n")) != NULL) {
          *c = '\0';
          isAuthorized = strstr(head, CLIENT_PHONE_NUMBER) != NULL;
          if (isAuthorized) {
            for (body = c + 1; *body != '\0' && *body <= ' '; body++);
            for (c = strchr(body, '\0') - 1; *c <= ' '; c--) *c = '\0';
            c = strchr(body, '\0');
            if (c != NULL && (c -= 2) >= body && streq(c, "OK"))
              for (*c = '\0'; *c <= ' '; c--) *c = '\0';
          } else {
            Serial.print(F("Unauthorized message: "));
            Serial.println(modemData);
          }
        } else {
          Serial.print(F("Read message failure: "));
          Serial.println(modemData);
        }
      } else {
        Serial.print(F("Read message failure: "));
        Serial.println(modemData);
      }

      if (body != NULL) {
        for (c = body; *c != '\0'; c++)
          if (*c >= 'A' && *c <= 'Z') *c -= 'A' - 'a';

        Serial.print(F("Received command: "));
        Serial.println(body);

        if (streq(body, "sms on")) smsOnStatusChange = true;
        if (streq(body, "sms off")) smsOnStatusChange = false;

        vin = readVinput() + 50;
        Serial.println(F("Sending SMS"));
        sprintf(sms, "Alarm status is %s\n"
                     "SMS info is %s\n"
                     "V input: %d.%dV",
               ((alarmStatus == STATUS_DISARM) ?
		             "DISARM" : ((alarmStatus == STATUS_ARM) ? "ARM" : "PANIC")),
		           ((smsOnStatusChange) ? "on" : "off"),
		           vin / 1000, vin % 1000 / 100);
        sendSms(sms);
      }

      Serial.print(F("Deleting SMS #"));
      Serial.println(msgN);
      sprintf(cmd, "AT+CMGD=%d", msgN);
      modemSendCommand(modem, cmd, "OK");

      modemSendCommand(modem, "AT+CNMI=2,1", "OK");
    } else {
      Serial.print(F("Unable to read message number: "));
      Serial.println(modemData);
    }
  } else {
    Serial.print(F("Modem data arrived: "));
    Serial.println(modemData);
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

void showAlarmStatus() {
  switch (alarmStatus) {
    case STATUS_DISARM:
      Serial.println(F("Alarm status: disarm"));
      break;
    case STATUS_ARM:
      Serial.println(F("Alarm status: arm"));
      break;
    case STATUS_PANIC:
      Serial.println(F("Alarm status: panic"));
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
      if (smsOnStatusChange)
        sendSms("Alarm status changed to DISARM");
      break;
    case STATUS_ARM:
      if (smsOnStatusChange)
        sendSms("Alarm status changed to ARM");
      break;
    case STATUS_PANIC:
      if (smsOnStatusChange)
        sendSms("Alarm status changed to PANIC");
      call();
      break;
  }
}

void sendSms(const char *text) {
  char cmd[30];

  modemSendCommand(modem, "ATH", "OK");
  delay(50);
  
  sprintf(cmd, "AT+CMGS=\"%s\"", CLIENT_PHONE_NUMBER);
  modemSendCommand(modem, cmd, NULL);
  modem.print(text);
  modem.print((char)26);
  modemReadData(modem, modemData, sizeof(modemData)/ sizeof(char) - 1);

  Serial.print(F("Send SMS: "));
  Serial.println(modemData);
}

void call() {
  char cmd[30];
  
  sprintf(cmd, "ATD%s;", CLIENT_PHONE_NUMBER);
  modemSendCommand(modem, cmd, NULL);

  Serial.print(F("Call: "));
  Serial.println(modemData);
}

void showModemStatus() {
  modemSendCommand(modem, "AT+CPAS", NULL);
  Serial.print(F("Modem status: "));
  Serial.println(modemData);
}

void showModemReg() {
  modemSendCommand(modem, "AT+CREG?", NULL);
  Serial.print(F("Modem registration: "));
  Serial.println(modemData);
}

void modemShutdown() {
  modemSendCommand(modem, "AT+CPWROFF", NULL);
  Serial.print(F("Modem shutdown: "));
  Serial.println(modemData);
}

void modemHangup() {
  modemSendCommand(modem, "ATH", NULL);
  Serial.print(F("Modem hangup: "));
  Serial.println(modemData);
}

void showVinput() {
  char buf[20];
  unsigned int vin = readVinput() + 50;
  sprintf(buf, "V input: %d.%dV", vin / 1000, vin % 1000 / 100);
  Serial.println(buf);
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

// vim:et:ci:pi:sts=0:sw=2:ts=2
