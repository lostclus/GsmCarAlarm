// TODO: спящий режим GSM модуля
// TODO: измерение входного напряжения
// TODO: запасное питание от кроны
// TODO: пересылка SMS
#include <SoftwareSerial.h>

#define LED_PIN 13
#define ALARM_ALARM_PIN 4
#define ALARM_LED_PIN 5

#define STATUS_DISARM 0
#define STATUS_ARM 1
#define STATUS_PANIC 2

const String CLIENT_PHONE_NUMBER = "0956182556";

SoftwareSerial modem(2, 3);
int alarmStatus = STATUS_DISARM;
boolean smsOnStatusChange = false;

boolean ledStatus = false;
unsigned long ledChangeTime = 0;
int alarmAlarmStatus = LOW;
unsigned long alarmAlarmChangeTime = 0;
int alarmAlarmShortImpulseCount = 0;
unsigned long alarmLedHighTime = 0;
unsigned long modemInitTime = 0;


void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(ALARM_ALARM_PIN, INPUT);
  pinMode(ALARM_LED_PIN, INPUT);
  
  delay(2000);
  digitalWrite(LED_PIN, LOW);
  
  Serial.begin(9600);
  Serial.println("GSM car alarm module");

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
  
  if (digitalRead(ALARM_LED_PIN) == HIGH) {
    alarmLedHighTime = currentTime;
  }
}

void monitorControl() {
  String input;
  
  if (!Serial.available()) return;
  
  input = Serial.readString();
  input.trim();
  
  if (input == "alarm status") showAlarmStatus();
  if (input == "alarm status disarm") setAlarmStatus(STATUS_DISARM);
  if (input == "alarm status arm") setAlarmStatus(STATUS_ARM);
  if (input == "alarm status panic") setAlarmStatus(STATUS_PANIC);
  if (input == "modem status") showModemStatus();
  if (input == "modem reg") showModemReg();
  if (input == "modem hangup") modemHangup();
  if (input == "modem shutdown") modemShutdown();
  if (input == "sms on") smsOnStatusChange = true;
  if (input == "sms off") smsOnStatusChange = false;
  if (input == "test call") call();
  if (input == "test sms") sendSms("test SMS");
}

void modemInit() {
  String resp;
  
  Serial.println("Initializing modem...");
  modem.begin(9600);
  modem.setTimeout(1000);
  
  modem.println("ATH");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("ATH failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("ATE0");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("ATE0 failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("AT+CLIP=1");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("AT+CLIP=1 failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("AT+CMGF=1");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("AT+CMGF=1 failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("AT+CSCS=\"GSM\"");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("AT+CSCS=\"GSM\" failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("AT+CMGD=1,4");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("AT+CMGD=1,4 failed!\n"
                   "---\n" + resp + "\n---");
  delay(50);
  modem.println("AT+CNMI=2,1");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  if (resp.indexOf("OK") == -1)
    Serial.println("AT+CNMI=2,1 failed!\n"
                   "---\n" + resp + "\n---");
  Serial.println("done");

  modem.setTimeout(1000);

  modemInitTime = millis();
}

void modemControl() {
  String data;
  
  if (!modem.available()) {
    if ((millis() - modemInitTime) > (12 * 3600 * 1000L))
      modemInit();
    return;
  }
  
  data = modem.readString();
  
  if (data.indexOf("RING") >= 0) {
    Serial.println("Ring detected");
    boolean isAuthorized = data.indexOf(CLIENT_PHONE_NUMBER) >= 0;
    modem.println("ATH");
    modem.readString();
    if (isAuthorized) sendAlarmStatus();
  } else if (data.indexOf("+CMTI:")) {
    Serial.println("New SMS arrived");
    int i = data.indexOf(",");
    if (i > 0) i++;
    int j = i;
    while (j > 0 && j < data.length() && isDigit(data.charAt(j))) j++;
    if (i > 0 && j > i) {
      String msgN = data.substring(i, j);
      modem.println("AT+CNMI=2,0");
      for (int readCnt = 0; (data = modem.readString()) == "" && readCnt < 10; readCnt++);
      if (data.indexOf("OK") == -1)
      Serial.println("AT+CNMI=2,0 failed!\n"
                     "---\n" + data + "\n---");
      Serial.println("Reading SMS #" + msgN);
      modem.println("AT+CMGR=" + msgN);
      for (int readCnt = 0; (data = modem.readString()) == "" && readCnt < 10; readCnt++);
      data.trim();
      if (data.endsWith("OK")) {
        int n = data.indexOf("\n");
          if (n > 0) {
          String header = data.substring(0, n);
          if (header.indexOf(CLIENT_PHONE_NUMBER) > 0) {
            String body = data.substring(n + 1, data.lastIndexOf("OK"));
            body.trim();
            body.toLowerCase();
            if (body == "sms on") smsOnStatusChange = true;
            if (body == "sms off") smsOnStatusChange = false;
            /* if (body == "status") sendAlarmStatus(); */
            sendSms("Alarm status is " +
	            String((alarmStatus == STATUS_DISARM) ?
		           "DISARM" : ((alarmStatus == STATUS_ARM) ? "ARM" : "PANIC")) + "\n"
                    "SMS info is " + String(((smsOnStatusChange) ? "on" : "off")));
          } else {
            Serial.println("Unauthorized message");
          }
        } else {
          Serial.println("Unable to read message header: " + data);
        }
      } else {
        Serial.println("Read message failure: " + data);
      }
      modem.println("AT+CMGD=" + msgN);
      for (int readCnt = 0; (data = modem.readString()) == "" && readCnt < 10; readCnt++);
      if (data.indexOf("OK") == -1)
      Serial.println("AT+CMGD=" + msgN + " failed!\n"
                     "---\n" + data + "\n---");
      modem.println("AT+CNMI=2,1");
      for (int readCnt = 0; (data = modem.readString()) == "" && readCnt < 10; readCnt++);
      if (data.indexOf("OK") == -1)
      Serial.println("AT+CNMI=2,1 failed!\n"
                     "---\n" + data + "\n---");
    } else {
      Serial.println("Unable to read message number: " + data);
    }
  } else {
    Serial.println("Modem data arrived: " + data);
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
      Serial.println("Alarm status: disarm");
      break;
    case STATUS_ARM:
      Serial.println("Alarm status: arm");
      break;
    case STATUS_PANIC:
      Serial.println("Alarm status: panic");
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

void sendSms(String text) {
  String resp;
  
  modem.println("ATH");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  delay(50);
  
  modem.println("AT+CMGS=\"" + CLIENT_PHONE_NUMBER + "\"");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  modem.print(text);
  modem.print((char)26);
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Send SMS: " + resp);
}

void call() {
  String resp;
  
  modem.println("ATD" + CLIENT_PHONE_NUMBER + ";");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Call: " + resp);
}

void showModemStatus() {
  String resp;
  
  modem.println("AT+CPAS");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Modem status: " + resp);
}

void showModemReg() {
  String resp;
  
  modem.println("AT+CREG?");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Modem registration: " + resp);
}

void modemShutdown() {
  String resp;
  
  modem.println("AT+CPWROFF");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Modem shutdown: " + resp);
}

void modemHangup() {
  String resp;
  
  modem.println("ATH");
  for (int readCnt = 0; (resp = modem.readString()) == "" && readCnt < 10; readCnt++);
  resp.trim();
  Serial.println("Modem hangup: " + resp);
}

// vim:et:ci:pi:sts=0:sw=2:ts=2
