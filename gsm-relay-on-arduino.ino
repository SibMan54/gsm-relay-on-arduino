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
int8_t heaterVal = 1;               // Состояние самоподогрева
#endif
volatile unsigned long lastPressTime = 0; // Переменная для защиты от дребезга

//---------АДРЕСА В EEPROM--------------
#define STAT_ADDR 1
#define TIMER_ADDR 2
#define HEAT_ADDR 3

enum Command {
    CMD_RELAY_ON,
    CMD_RELAY_OFF,
    CMD_TIMER,
    CMD_TIMER_OFF,
    CMD_HEATING,
    CMD_HEATING_OFF,
    CMD_TEMPERATURE,
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
  sendAtCmd("AT+IPR=9600");
  sendAtCmd("AT+CLIP=1");
  sendAtCmd("AT+CMGF=1");
  sendAtCmd("AT+CSCS=\"GSM\"");
  sendAtCmd("AT+CNMI=2,2");
  sendAtCmd("AT+CMGD=1,4");
}

//--------------------------------------------------------------
// Обработчик прерывания кнопки с защитой от дребезга
//--------------------------------------------------------------
void buttonISR() {
  uint32_t currentTime = millis();
  if (currentTime - lastPressTime > 200) {  // 200 мс защита от дребезга
    switchRelay(!state);
    lastPressTime = currentTime;
  }
}

//---------------------------------------------------
// Процедура отправки СМС
//---------------------------------------------------
void sms(String text, String phone)
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

  MASTER = read_eeprom_number(10);
  MASTER2 = read_eeprom_number(30);
}

//--------------------------------------------------------------
// Основной цикл loop
//--------------------------------------------------------------
void loop() {
  if (mySerial.available()) incoming_call_sms();
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
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("delete sms") > -1) return CMD_DELETE_SMS;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay on") > -1 && !state) return CMD_RELAY_ON;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay off") > -1 && state) return CMD_RELAY_OFF;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("timer ") > -1) return CMD_TIMER;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("timer off") > -1) return CMD_TIMER_OFF;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("heating ") > -1) return CMD_HEATING;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("heating off") > -1) return CMD_HEATING_OFF;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("temper") > -1) return CMD_TEMPERATURE;
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
        case CMD_RELAY_ON:
            switchRelay(true);
            sms("RELAY ON OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
        case CMD_RELAY_OFF:
            switchRelay(false);
            sms("RELAY OFF OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
        case CMD_TEMPERATURE:
            sms("Temperature: " + String(currentTemper()) + "'C", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
        case CMD_DELETE_SMS:
            sms("Delete SMS OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            val = "";
            delay(1000);
            sendAtCmd("AT+CMGD=1,4");
            break;
        case CMD_NEW_MASTER:
            MASTER = val.substring(10, 21);
            update_eeprom_number(10,MASTER);
            sms("Master Nomer izmenen", MASTER);
            break;
        case CMD_NEW_MASTER2:
            MASTER2 = val.substring(10, 21);
            update_eeprom_number(30,MASTER2);
            sms("Master2 Nomer izmenen", MASTER2);
            break;
#ifdef USE_readNumberSIM
        case CMD_SIM_MASTER:
            val = "";
            readNumberSIM();
            sms("Master Nomer izmenen", MASTER);
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
                    sms("TIMER ON " + timerTmp + " MIN", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                }
                else {
                    EEPROM.update(TIMER_ADDR, false);
                    sms("TIMER OFF", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                }
                val = "";
            }
            break;
        case CMD_TIMER_OFF:
            timer = 0;
            EEPROM.update(TIMER_ADDR, false);
            sms("TIMER OFF OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
#endif
#ifdef USE_HEATING
        case CMD_HEATING:
            {
                String heatTmp = val.substring(58);
                heaterVal = heatTmp.toInt();
                EEPROM.update(HEAT_ADDR, heaterVal);
                if (heaterVal < 1) {
                    sms("HEATING ON " + String(heaterVal) + "'C", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                }
                else sms("HEATING OFF, TEMP > +1 C", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                val = "";
            }
            break;
        case CMD_HEATING_OFF:
            heaterVal = 1;
            EEPROM.update(HEAT_ADDR, heaterVal);
            sms("HEATING OFF OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
#endif
        case CMD_RING:
            if (val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) {
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
