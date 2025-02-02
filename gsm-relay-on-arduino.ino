//******************************************************************************************
// Проект GSM реле на Arduino с использованием модуля NEOWAY M590
//******************************************************************************************

#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <OneWire.h>

SoftwareSerial mySerial(A2, A3);  // RX, TX программного порта

// #define USE_readNumberSIM    // Раскоментировать для считывания номера из SIM карты
// #define USE_HEATING          // Раскоментировать для включения самоподогрева
#define USE_TIMER            // Закомментировать, если не нужен таймер

#define CHECK_NUMBER (val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1)
#define NUMBER_TO_SEND (val.indexOf(MASTER) > -1) ? MASTER : MASTER2)

//---------КОНТАКТЫ--------------
#define POWER 2                 // Реле питания
#define STAT_LED 3              // Светодиод состояния
#define BUTTON 4                // Кнопка управления
#define HEATER 6                // Подогреватель
#define DS18B20 7               // Датчик температуры
#define DS_POWER_MODE 1         // Режим питания датчика
OneWire sensDs(DS18B20);

//---------ПЕРЕМЕННЫЕ И КОНСТАНТЫ--------------
const String MASTER = "79123456789";  // Основной мастер-номер
const String MASTER2 = "79123456789"; // Доп. мастер-номер
String val = "";
bool state = false;             // Текущее состояние реле
byte bufData[9];                // Буфер данных для термодатчика
#ifdef USE_TIMER
uint32_t timer = 0;             // Таймер работы реле
#endif
#ifdef USE_HEATING
int8_t heaterVal = 1;                   // Состояние самоподогрева
#endif
volatile uint32_t lastPressTime = 0;    // Переменная для защиты от дребезга
volatile uint8_t int0Flag=false;       // Флаг прерывания по нажатию кнопки

//---------АДРЕСА В EEPROM--------------
#define STAT_ADDR 1
#define TIMER_ADDR 2
#define HEAT_ADDR 3

enum Command {
    CMD_STATUS,
    CMD_TEMPERATURE,
    CMD_RELAY_ON,
    CMD_RELAY_OFF,
    CMD_TIMER,
    CMD_TIMER_OFF,
    CMD_HEATING,
    CMD_HEATING_OFF,
    CMD_NEW_MASTER,
    CMD_NEW_MASTER2,
    CMD_SIM_MASTER,
    CMD_DELETE_SMS,
    CMD_RING,
    CMD_UNKNOWN
};

//--------------------------------------------------------------
// Функция отправки команды модему
//--------------------------------------------------------------
bool sendAtCmd(String at_send, String ok_answer = "OK", uint16_t wait_sec = 2) {
  mySerial.println(at_send);
  uint32_t exit_ms = millis() + wait_sec * 1000;
  String answer;
  while (millis() < exit_ms) {
    if (mySerial.available()) {
      answer = mySerial.readString();
      if (answer.indexOf(ok_answer) > -1) return true;
    }
  }
  return false;
}
/*
bool sendATCommand(const char* command, const char* expectedResponse, unsigned long timeout = 2000) {
  mySerial.println(command);
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (mySerial.find(expectedResponse)) {
      return true;
    }
  }
  return false;
}
*/

//--------------------------------------------------------------
// Инициализация GSM модема
//--------------------------------------------------------------
void initGSM() {
  sendAtCmd("AT+IPR=9600");                 // команда модему на установку скорости
  sendAtCmd("AT+CLIP=1");                   // включаем АОН
  sendAtCmd("AT+CMGF=1");                   // режим кодировки СМС - обычный (для англ.)
  sendAtCmd("AT+CSCS=\"GSM\"");             // режим кодировки текста
  sendAtCmd("AT+CNMI=2,2");                 // отображение смс в терминале сразу после приема (без этого сообщения молча падают в память)
  do {
    sendAtCmd("AT+CSQ");  // Запрашиваем уровень сигнала (если 99, то связи нет)
    digitalWrite(STAT_LED, !digitalRead(STAT_LED));  // Мигание светодиодом
    delay(250);
  } while (!mySerial.find("+PBREADY") || mySerial.find("+CSQ: 99"));  // Проверяем статус соединения
  digitalWrite(STAT_LED, false);            // гасим Светодиод
  sendAtCmd("AT+CMGD=1,4");                 // стереть все старые сообщения
}

//--------------------------------------------------------------
// Обработчик прерывания кнопки с защитой от дребезга
//--------------------------------------------------------------
void buttonISR() {
  int0Flag=true;
  lastPressTime = millis();
}

//---------------------------------------------------
// Процедура отправки СМС
//---------------------------------------------------
void sendSMS(String text, String phone)
{
  mySerial.println("AT+CMGS=\"+" + phone + "\"");
  delay(500);
  mySerial.print(text);
  delay(500);
  mySerial.print((char)26);
  delay(5000);
}

//--------------------------------------------------------------
// Чтение номера из СИМ
//--------------------------------------------------------------
#ifdef USE_readNumberSIM
void readNumberSIM() {
  byte ch = 0;
  byte x = 0;
  while (mySerial.available()) mySerial.read();
  delay(100);
  mySerial.println("AT+CPBF=\"MASTER\"");     //чтение номера из СИМ
  delay(300);
  while (mySerial.available()) {         //сохраняем входную строку в переменную val
    ch = mySerial.read();
    x++;
    if((x > 30) && (x < 42)) {
      val += char(ch);
      delay(5);
    }
   }
   if (val.indexOf("79") > -1 && val.length() == 11) {
     MASTER = val;
   }

   ch = 0;
   val = "";
   update_master_eeprom(10);
}
#endif

//--------------------------------------------------------------
// Чтение номера из EEPROM
//--------------------------------------------------------------
String read_eeprom_number(int addr) {
  String number = "";
  for (byte i = 0; i < 12; i++) number += char(EEPROM.read(addr + i));
  return number;
}

//--------------------------------------------------------------
// Запись номера в EEPROM
//--------------------------------------------------------------
void update_eeprom_number(int addr, String num) {
  for (byte i = 0; i < 12; i++) EEPROM.write(addr + i, num[i]);
}

//--------------------------------------------------------------
// Управление реле
//--------------------------------------------------------------
void switchRelay(bool newState) {
  digitalWrite(POWER, newState);
  digitalWrite(STAT_LED, newState);
  EEPROM.update(STAT_ADDR, newState);
  state = newState;
}

//--------------------------------------------------------------
// Измерение температуры
//--------------------------------------------------------------
float currentTemper() {
  sensDs.reset();
  sensDs.write(0xCC, DS_POWER_MODE);
  sensDs.write(0x44, DS_POWER_MODE);
  delay(900);
  sensDs.reset();
  sensDs.write(0xCC, DS_POWER_MODE);
  sensDs.write(0xBE, DS_POWER_MODE);
  sensDs.read_bytes(bufData, 9);
  if (OneWire::crc8(bufData, 8) != bufData[8]) return -127.0;
  return (float)((int)bufData[0] | (((int)bufData[1]) << 8)) * 0.0625 + 0.03125;
}

//--------------------------------------------------------------
// Функция работы таймера
//--------------------------------------------------------------
#ifdef USE_TIMER
void timerControl() {
  if (timer = 0 || millis() >= timer) {
    switchRelay(false);
    timer = 0;
    EEPROM.update(TIMER_ADDR, false);
  }
}
#endif

//--------------------------------------------------------------
// Функция управления самоподогревом
//--------------------------------------------------------------
#ifdef USE_HEATING
void heaterControl() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck >= 10000) {  // проверка температуры раз в 10 сек
    lastCheck = millis();
    float temp = currentTemper();
    digitalWrite(HEATER, temp < heaterVal ? HIGH : LOW);
  }
}
#endif

//--------------------------------------------------------------
// ИНИЦИАЛИЗАЦИЯ УСТРОЙСТВА
//--------------------------------------------------------------
void setup() {
  pinMode(POWER, OUTPUT);
  pinMode(STAT_LED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(HEATER, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(BUTTON), buttonISR, FALLING); // Настройка аппаратного прерывания

  mySerial.begin(9600);
  initGSM();

  state = EEPROM.read(STAT_ADDR);
  switchRelay(state);
  digitalWrite(STAT_LED, state);
  #ifdef USE_TIMER
  timer = EEPROM.read(TIMER_ADDR);
  #endif
  #ifdef USE_HEATING
  heaterVal = EEPROM.read(HEAT_ADDR);
  #endif

  if (read_eeprom_number(10).indexOf("79") > -1 && read_eeprom_number(10).length() == 11) {
    MASTER = read_eeprom_number(10);
  }
  if (read_eeprom_number(30).indexOf("79") > -1 && read_eeprom_number(30).length() == 11) {
    MASTER2 = read_eeprom_number(30);
  }
}

//--------------------------------------------------------------
// Основной цикл loop
//--------------------------------------------------------------
void loop() {
  if (mySerial.available()) incoming_call_sms();
  if(int0Flag==true && (millis() - lastPressTime > 200)) {
    switchRelay(!state);
    int0Flag=false;
    lastPressTime = 0;
  }
  #ifdef USE_HEATING
  if(heaterVal<1) heaterControl();
  #endif
  #ifdef USE_TIMER
  if(timer!=0) timerControl();
  #endif
}


//-----------------------------------------------------------------------------
// Процедура обработки звонков и смс
//-----------------------------------------------------------------------------

Command getCommand(const String& val) {
    if (val.indexOf("+CMT") > -1) {
        if (CHECK_NUMBER && val.indexOf("delete sms") > -1) return CMD_DELETE_SMS;
        if (CHECK_NUMBER && val.indexOf("relay on") > -1 && !state) return CMD_RELAY_ON;
        if (CHECK_NUMBER && val.indexOf("relay off") > -1 && state) return CMD_RELAY_OFF;
        if (CHECK_NUMBER && val.indexOf("timer ") > -1) return CMD_TIMER;
        if (CHECK_NUMBER && val.indexOf("timer off") > -1) return CMD_TIMER_OFF;
        if (CHECK_NUMBER && val.indexOf("heating ") > -1) return CMD_HEATING;
        if (CHECK_NUMBER && val.indexOf("heating off") > -1) return CMD_HEATING_OFF;
        if (CHECK_NUMBER && val.indexOf("temper") > -1) return CMD_TEMPERATURE;
        if (CHECK_NUMBER && val.indexOf("status") > -1) return CMD_STATUS;
        if (val.indexOf("new master") > -1) return CMD_NEW_MASTER;
        if (val.indexOf("new master2") > -1) return CMD_NEW_MASTER2;
        if (val.indexOf("SIM master N") > -1) return CMD_SIM_MASTER;
    }
    if (val.indexOf("RING") > -1) return CMD_RING;
    return CMD_UNKNOWN;
}

void incoming_call_sms() {
    byte ch = 0;
    delay(200);
    while (mySerial.available()) {
        ch = mySerial.read();
        val += char(ch);
        delay(10);
    }

    Command cmd = getCommand(val);
    switch (cmd) {
        case CMD_STATUS:
            sendSMS("RELAY: " + String(state ? "ON" : "OFF") + ", TEMP: " + String(currentTemper()) + "'C", NUMBER_TO_SEND;
            break;
        case CMD_TEMPERATURE:
            sendSMS("Temperature: " + String(currentTemper()) + "'C", NUMBER_TO_SEND;
            break;
        case CMD_RELAY_ON:
            switchRelay(true);
            sendSMS("RELAY ON OK", NUMBER_TO_SEND;
            break;
        case CMD_RELAY_OFF:
            switchRelay(false);
            sendSMS("RELAY OFF OK", NUMBER_TO_SEND;
            break;
        case CMD_DELETE_SMS:
            sendSMS("Delete SMS OK", NUMBER_TO_SEND;
            val = "";
            delay(1000);
            sendAtCmd("AT+CMGD=1,4");
            break;
        case CMD_NEW_MASTER:
            MASTER = val.substring(10, 21);
            update_eeprom_number(10,MASTER);
            sendSMS("Master Nomer izmenen", MASTER);
            break;
        case CMD_NEW_MASTER2:
            MASTER2 = val.substring(10, 21);
            update_eeprom_number(30,MASTER2);
            sendSMS("Master2 Nomer izmenen", MASTER2);
            break;
#ifdef USE_readNumberSIM
        case CMD_SIM_MASTER:
            val = "";
            readNumberSIM();
            sendSMS("Master Nomer izmenen", MASTER);
            break;
#endif
#ifdef USE_TIMER
        case CMD_TIMER:
            {
                String timerTmp = val.substring(54);
                timer = timerTmp.toInt() * 60000 + millis();
                if (timer != 0) {
                    switchRelay(true);
                    EEPROM.update(TIMER_ADDR, true);
                    sendSMS("TIMER ON " + timerTmp + " MIN", NUMBER_TO_SEND;
                }
                else {
                    EEPROM.update(TIMER_ADDR, false);
                    sendSMS("TIMER OFF", NUMBER_TO_SEND;
                }
                val = "";
            }
            break;
        case CMD_TIMER_OFF:
            timer = 0;
            EEPROM.update(TIMER_ADDR, false);
            sendSMS("TIMER OFF OK", NUMBER_TO_SEND;
            break;
#endif
#ifdef USE_HEATING
        case CMD_HEATING:
            {
                String heatTmp = val.substring(58);
                heaterVal = heatTmp.toInt();
                EEPROM.update(HEAT_ADDR, heaterVal);
                if (heaterVal < 1) {
                    sendSMS("HEATING ON " + String(heaterVal) + "'C", NUMBER_TO_SEND;
                }
                else sendSMS("HEATING OFF, TEMP > +1 C", NUMBER_TO_SEND;
                val = "";
            }
            break;
        case CMD_HEATING_OFF:
            heaterVal = 1;
            EEPROM.update(HEAT_ADDR, heaterVal);
            sendSMS("HEATING OFF OK", NUMBER_TO_SEND;
            break;
#endif
        case CMD_RING:
            if CHECK_NUMBER {
                delay(500);
                switchRelay(!state);
                sendAtCmd("ATH0");
            } else {
                delay(10500);
                sendAtCmd("ATH");
            }
            break;
        default:
            val = "";
            break;
    }
}
