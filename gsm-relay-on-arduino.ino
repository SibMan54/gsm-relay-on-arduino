//******************************************************************************************
// Проект GSM реле на Arduino с использованием модуля NEOWAY M590
//******************************************************************************************

#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <OneWire.h>

//---------ОТЛАДКА--------------
// #define DEBUG_ENABLE                // Раскомментируй, чтобы включить отладку
#ifdef DEBUG_ENABLE
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINT(x) Serial.print(x)
#else
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINT(x)
#endif

//---------Выбор модема--------------
#define USE_M590    // Раскомментируйте для использования NEOWAY M590
// #define USE_SIM800 // Раскомментируйте для использования SIM800

//---------НАСТРОЙКА--------------
#define DS_POWER_MODE 1             // Режим питания датчика
#define USE_TIMER                   // Закомментировать, если не нужен таймер
// #define USE_READ_NUM_SIM         // Раскоментировать для считывания номера из SIM карты
// #define USE_HEATING              // Раскоментировать для включения самоподогрева
#define LINE_BREAK "\n"  // или "\r\n" (Тип переноса строки в СМС)

//---------КОНТАКТЫ--------------
SoftwareSerial mySerial(10, 11);    // RX, TX для для связи с модемом
#define POWER 12                    // Реле питания
#define STAT_LED 13                 // Светодиод состояния
#define BUTTON 2                    // Кнопка управления
#define HEATER 6                    // Подогреватель
#define DS18B20 7                   // Датчик температуры
OneWire sensDs(DS18B20);            // Инициализация шины 1-Wire для работы датчика

//---------ПЕРЕМЕННЫЕ--------------
String MASTER = "79123456789";          // Основной мастер-номер
String MASTER2 = "79123456789";         // Второй мастер-номер
String val = "";
bool state = false;                     // Текущее состояние реле
bool answerSMS = false;
byte bufData[9];                        // Буфер данных для термодатчика
#ifdef USE_TIMER
uint32_t timer = 0;                     // Таймер работы реле
#endif
#ifdef USE_HEATING
int8_t heaterVal = 1;                   // Состояние самоподогрева
#endif
volatile uint32_t lastPressTime = 0;    // Переменная для защиты от дребезга
volatile uint8_t btnFlag=false;        // Флаг прерывания по нажатию кнопки

//---------АДРЕСА В EEPROM--------------
#define STAT_ADDR 1
#define TIMER_ADDR 2
#define HEAT_ADDR 3
#define ASMS_ADDR 4

//---------СОКРАЦЕНИЯ--------------
#define CHECK_NUMBER (val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1)
#define NUMBER_TO_SEND (val.indexOf(MASTER) > -1) ? MASTER : MASTER2

enum Command {
    CMD_STATUS,
    CMD_TEMPERATURE,
    CMD_RELAY_ON,
    CMD_RELAY_OFF,
    CMD_TIMER,
    CMD_TIMER_OFF,
    CMD_HEATING,
    CMD_HEATING_OFF,
    CMD_ANSWER_SMS,
    CMD_NEW_MASTER,
    CMD_NEW_MASTER2,
    CMD_SIM_MASTER,
    CMD_CLEAR,
    CMD_RING,
    CMD_UNKNOWN
};

//--------------------------------------------------------------
// Функция отправки команды модему
//--------------------------------------------------------------
bool sendAtCmd(String at_send, String ok_answer = "OK", uint16_t wait_sec = 2) {
  mySerial.println(at_send);
  uint32_t exit_ms = millis() + wait_sec * 1000;
  String answer = "";

  while (millis() < exit_ms) {
    while (mySerial.available()) {
      char c = mySerial.read();
      answer += c;
      DEBUG_PRINT(c);      // Выводим ответ модема в монитор порта
      // Обрезаем лишние символы (если в ответе есть \r\nOK\r\n)
      answer.trim();
      if (answer.indexOf(ok_answer) > -1) {
          // DEBUG_PRINTLN("\nOK received!");  // Сообщаем, что получили подтверждение
        return true;
      }
    }
  }
  DEBUG_PRINTLN("\nERROR: No response"); // Сообщаем об ошибке
  return false;
}

//--------------------------------------------------------------
// Инициализация GSM модема
//--------------------------------------------------------------
bool initModem() {
    mySerial.begin(9600);
    delay(2000);

#ifdef USE_M590
    digitalWrite(STAT_LED, HIGH);  // Включаем светодиод при старте
    DEBUG_PRINTLN("Ожидание готовности модема...");

    while (!mySerial.find("PBREADY")) {  // Ждём сообщение "+PBREADY"
        digitalWrite(STAT_LED, !digitalRead(STAT_LED));  // Мигание светодиодом
        delay(500);
    }

    digitalWrite(STAT_LED, LOW);  // Готовность, выключаем светодиод
    DEBUG_PRINTLN("Модем готов!");

    if (sendAtCmd("AT+IPR=9600")) DEBUG_PRINTLN("Скорость 9600 задана");                    // команда модему на установку скорости
    delay(1000);
    if (sendAtCmd("AT+CLIP=1")) DEBUG_PRINTLN("АОН включен");                               // включаем АОН
    if (sendAtCmd("AT+CMGF=1")) DEBUG_PRINTLN("Режим SMS установлен");                      // режим кодировки СМС - обычный (для англ.)
    if (sendAtCmd("AT+CSCS=\"GSM\"")) DEBUG_PRINTLN("Кодировка текста установлена");        // режим кодировки текста
    if (sendAtCmd("AT+CNMI=2,2")) DEBUG_PRINTLN("Настройки отображения SMS установлены");   // отображение смс в терминале сразу после приема (без этого сообщения молча падают в память)
    if (sendAtCmd("AT&W")) DEBUG_PRINTLN("Настройки сохранены");                            // сохранение настроек в энергонезависимой памяти
    delay(300);

    // Запрашиваем уровень сигнала
    mySerial.println("AT+CSQ");
    unsigned long startTime = millis();
    String response = "";

    while (millis() - startTime < 2000) {  // Ждем 2 секунды
        if (mySerial.available()) {
            char c = mySerial.read();
            response += c;
        }
    }

    int csq = -1;
    int index = response.indexOf("+CSQ: ");
    if (index != -1) {
        csq = response.substring(index + 6).toInt();
    }

    DEBUG_PRINT("\nУровень сигнала: ");
    DEBUG_PRINTLN(csq);

    if (csq < 10 || csq > 31) {
        DEBUG_PRINTLN("Ошибка: слабый сигнал!");
        return false;
    }

    DEBUG_PRINTLN("\nМодем успешно инициализирован");
    return true;
#endif

#ifdef USE_SIM800
    sendAtCmd("AT");
    sendAtCmd("AT+CMGF=1");
    sendAtCmd("AT+CNMI=1,2,0,0,0"); // Настройка приема SMS
    sendAtCmd("AT+CSMP=17,167,0,0"); // Настройка кодировки SMS
#endif
}

//--------------------------------------------------------------
// Обработчик прерывания кнопки с защитой от дребезга
//--------------------------------------------------------------
void buttonISR() {
  btnFlag=true;
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
#ifdef USE_READ_NUM_SIM
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
     update_eeprom_number(10, MASTER);
   }

   val = "";
}
#endif

//--------------------------------------------------------------
// Чтение номера из EEPROM
//--------------------------------------------------------------
String read_eeprom_number(int addr) {
    String num = "";
    char ch;

    for (int i = 0; i < 11; i++) {  // Читаем до 11 символов
        ch = EEPROM.read(addr + i);
        if (ch == '\0' || ch == 0xFF) break;  // Останавливаемся, если пусто
        num += ch;
    }

    // Проверяем, корректен ли номер
    if (num.length() == 11 && num.startsWith("79")) {
        return num;  // Если номер правильный, возвращаем его
    }
    else DEBUG_PRINTLN("\nНомер из EEPROM не считан");
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
  DEBUG_PRINTLN("\nРЕЛЕ: " + String(state ? "ON" : "OFF"));  // Сообщаем, что получили подтверждение
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
  #ifdef DEBUG_ENABLE
  Serial.begin(9600);
  #endif
  pinMode(POWER, OUTPUT);
  pinMode(STAT_LED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(HEATER, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(BUTTON), buttonISR, FALLING); // Настройка аппаратного прерывания

  initModem();

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
  DEBUG_PRINTLN("Мастер номер: " + MASTER + "Мастер номер 2: " + MASTER2);
}

//--------------------------------------------------------------
// Основной цикл loop
//--------------------------------------------------------------
void loop() {
  if (mySerial.available()) incoming_call_sms();
  if(btnFlag==true && (millis() - lastPressTime > 200)) {
    btnFlag=false;
    lastPressTime = 0;
    switchRelay(!state);
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
        if (CHECK_NUMBER && val.indexOf("clear") > -1) return CMD_CLEAR;
        if (CHECK_NUMBER && val.indexOf("relay on") > -1 && !state) return CMD_RELAY_ON;
        if (CHECK_NUMBER && val.indexOf("relay off") > -1 && state) return CMD_RELAY_OFF;
        if (CHECK_NUMBER && val.indexOf("timer ") > -1) return CMD_TIMER;
        if (CHECK_NUMBER && val.indexOf("timer off") > -1) return CMD_TIMER_OFF;
        if (CHECK_NUMBER && val.indexOf("heating ") > -1) return CMD_HEATING;
        if (CHECK_NUMBER && val.indexOf("heating off") > -1) return CMD_HEATING_OFF;
        if (CHECK_NUMBER && val.indexOf("temper") > -1) return CMD_TEMPERATURE;
        if (CHECK_NUMBER && val.indexOf("status") > -1) return CMD_STATUS;
        if (CHECK_NUMBER && val.indexOf("answer sms") > -1) return CMD_ANSWER_SMS;
        if (val.indexOf("new master") > -1) return CMD_NEW_MASTER;
        if (val.indexOf("new master2") > -1) return CMD_NEW_MASTER2;
        if (val.indexOf("sim master") > -1) return CMD_SIM_MASTER;
    }
    if (val.indexOf("RING") > -1) return CMD_RING;
    DEBUG_PRINTLN("Команда не найдена, возвращаем CMD_UNKNOWN.");
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

    val.toLowerCase();  // Приводим к нижнему регистру для единообразия
    DEBUG_PRINTLN("ВХОДЯЩЕЕ СОБЫТИЕ:" + val); // Выводим результат прочтения
    Command cmd = getCommand(val);
    switch (cmd) {
        case CMD_STATUS:
            sendSMS("RELAY: " + String(state ? "ON" : "OFF") + LINE_BREAK +
                    "TEMP: " + String(currentTemper()) + "'C" + LINE_BREAK +
                    "NUM1: " + MASTER + LINE_BREAK +
                    "NUM2: " + MASTER2, NUMBER_TO_SEND);
            DEBUG_PRINTLN("RELAY: " + String(state ? "ON" : "OFF") + LINE_BREAK +
                    "TEMP: " + String(currentTemper()) + "'C" + LINE_BREAK +
                    "NUM1: " + MASTER + LINE_BREAK +
                    "NUM2: " + MASTER2);
            break;
        case CMD_TEMPERATURE:
            sendSMS("Temperature: " + String(currentTemper()) + "'C", NUMBER_TO_SEND);
            DEBUG_PRINTLN("Temperature: " + String(currentTemper()) + "`C");
            break;
        case CMD_RELAY_ON:
            switchRelay(true);
            sendSMS("RELAY " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
            break;
        case CMD_RELAY_OFF:
            switchRelay(false);
            sendSMS("RELAY " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
            break;
        case CMD_CLEAR:
            sendSMS("CLEAR OK", NUMBER_TO_SEND);
            delay(1000);
            sendAtCmd("AT+CMGD=1,4");
            DEBUG_PRINTLN("Память очищена");
            break;
        case CMD_ANSWER_SMS:
            answerSMS = !answerSMS;
            EEPROM.update(ASMS_ADDR, answerSMS);
            sendSMS("ANSWER SMS: " + String(answerSMS ? "ON" : "OFF"), NUMBER_TO_SEND);
            DEBUG_PRINTLN("Отправка ответных СМС: " + String(answerSMS ? "ON" : "OFF"));
            break;
        case CMD_NEW_MASTER:
            MASTER = val.substring(10, 21);
            update_eeprom_number(10,MASTER);
            sendSMS("Osnovnoi nomer izmenen na: " + MASTER, MASTER);
            DEBUG_PRINTLN("Основной номер изменен на: " + MASTER);
            break;
        case CMD_NEW_MASTER2:
            MASTER2 = val.substring(10, 21);
            update_eeprom_number(30,MASTER2);
            sendSMS("Vtoroi nomer izmenen na: " + MASTER2, MASTER2);
            DEBUG_PRINTLN("Второй номер изменен на: " + MASTER2);
            break;
#ifdef USE_READ_NUM_SIM
        case CMD_SIM_MASTER:
            readNumberSIM();
            sendSMS("Osnovnoi nomer izmenen na: " + MASTER, MASTER);
            DEBUG_PRINTLN("Основной номер изменен на: " + MASTER);
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
                    sendSMS("TIMER ON " + timerTmp + " MIN", NUMBER_TO_SEND);
                    DEBUG_PRINTLN("Таймер вкл. на " + timerTmp + " MIN");
                }
                else {
                    EEPROM.update(TIMER_ADDR, false);
                    sendSMS("TIMER OFF", NUMBER_TO_SEND);
                    DEBUG_PRINTLN("Таймер выключен");
                }
            }
            break;
        case CMD_TIMER_OFF:
            timer = 0;
            EEPROM.update(TIMER_ADDR, false);
            sendSMS("TIMER OFF OK", NUMBER_TO_SEND);
            DEBUG_PRINTLN("Таймер успешно выключен");
            break;
#endif
#ifdef USE_HEATING
        case CMD_HEATING:
            {
                String heatTmp = val.substring(58);
                heaterVal = heatTmp.toInt();
                EEPROM.update(HEAT_ADDR, heaterVal);
                if (heaterVal < 1) {
                    sendSMS("HEATING ON " + String(heaterVal) + "'C", NUMBER_TO_SEND);
                    DEBUG_PRINTLN("Подогрев вкл. при темп. " + String(heaterVal) + "'C");
                }
                else {
                    sendSMS("HEATING OFF, TEMP > +1 C`", NUMBER_TO_SEND);
                    DEBUG_PRINTLN("Подогрев выкл. т.к. темп. выше +1`C");
                }
            }
            break;
        case CMD_HEATING_OFF:
            heaterVal = 1;
            EEPROM.update(HEAT_ADDR, heaterVal);
            sendSMS("HEATING OFF OK", NUMBER_TO_SEND);
            DEBUG_PRINTLN("Подогрев успешно отключен");
            break;
#endif
        case CMD_RING:
            if CHECK_NUMBER {
                delay(500);
                switchRelay(!state);
                sendAtCmd("ATH0");
                if (answerSMS) {
                    sendSMS("RELAY " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
                }
            } else {
                delay(10500);
                sendAtCmd("ATH");
            }
            break;
        default:
            DEBUG_PRINTLN("Неизвестная команда");
            break;
    }
    val = "";
}
