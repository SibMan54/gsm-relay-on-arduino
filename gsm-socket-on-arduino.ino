//******************************************************************************************
// Проект GSM розетка на Arduino с использованием модуля NEOWAY M590
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
#define LINE_BREAK "\n"             // или "\r\n" (Тип переноса строки в СМС)

//---------КОНТАКТЫ--------------
SoftwareSerial gsmSerial(10, 11);   // RX, TX для для связи с модемом
#define POWER_PIN 12                // Реле питания
#define STATE_LED 13                // Светодиод состояния
#define BTN_PIN 2                   // Кнопка управления
#define RING_PIN 3                  // Пин, подключенный к RING-выходу модема
#define HEATER 6                    // Подогреватель
#define DS18B20 A3                   // Датчик температуры
OneWire sensDs(DS18B20);            // Инициализация шины 1-Wire для работы датчика

//---------ПЕРЕМЕННЫЕ--------------
String oneNum = "79123456789";          // Основной мастер-номер
String twoNum = "79123456789";          // Второй мастер-номер
String val = "";
bool state = false;                     // Текущее состояние нагрузки
bool saveState = false;                 // Переменная вкл/выкл сщхранения статуса нагрузки
bool replySMS = false;                  // Переменная вкл/выкл ответных СМС на звонок
byte bufData[9];                        // Буфер данных для термодатчика
#ifdef USE_TIMER
uint32_t timer = 0;                     // Таймер работы нагрузки
#endif
#ifdef USE_HEATING
int8_t heaterVal = 1;                   // Состояние самоподогрева
#endif
volatile uint32_t lastPressTime = 0;    // Переменная для защиты от дребезга
volatile bool btnFlag=false;            // Флаг прерывания по нажатию кнопки
volatile bool ringFlag = false;         // Флаг прерывания при поступлении смс или звонка

//---------АДРЕСА В EEPROM--------------
#define STATE_ADDR 1
#define SS_ADDR 2
#define REPL_ADDR 3
#define HEAT_ADDR 5

//---------СОКРАЦЕНИЯ--------------
#define CHECK_NUMBER (val.indexOf(oneNum) > -1 || val.indexOf(twoNum) > -1)
#define NUMBER_TO_SEND (val.indexOf(oneNum) > -1) ? oneNum : twoNum

enum Command {
    CMD_STATUS,
    CMD_TEMPERATURE,
    CMD_POWER_ON,
    CMD_POWER_OFF,
    CMD_TIMER,
    CMD_HEATING,
    CMD_HEATING_OFF,
    CMD_REPLY_SMS,
    CMD_SAVE_STATE,
    CMD_ONE_NUM,
    CMD_TWO_NUM,
    CMD_SIM_NUM,
    CMD_CLEAR,
    CMD_RING,
    CMD_UNKNOWN
};

//--------------------------------------------------------------
// Функция для индикации с помощью светодиода
//--------------------------------------------------------------
void indicateLed(int blinkCount, int pauseOnOff, int pauseTime) {
    pinMode(STATE_LED, OUTPUT); // Настраиваем пин светодиода как выход

    for (int i = 0; i < blinkCount; i++) {
        digitalWrite(STATE_LED, HIGH); // Включаем светодиод
        delay(pauseOnOff);                 // Ждем указанное время
        digitalWrite(STATE_LED, LOW);  // Выключаем светодиод
        delay(pauseOnOff);                // Ждем указанное время
    }

    delay(pauseTime); // Пауза между циклами
}

//--------------------------------------------------------------
// Функция отправки команды модему
//--------------------------------------------------------------
bool sendAtCmd(String at_send, String ok_answer = "OK", uint16_t wait_sec = 2) {
  gsmSerial.println(at_send);
  uint32_t exit_ms = millis() + wait_sec * 1000;
  String answer = "";

  while (millis() < exit_ms) {
    while (gsmSerial.available()) {
      char c = gsmSerial.read();
      answer += c;
      DEBUG_PRINT(c);      // Выводим ответ модема в монитор порта
      // Обрезаем лишние символы (если в ответе есть \r\nOK\r\n)
      answer.trim();
      if (answer.indexOf(ok_answer) > -1) {
          DEBUG_PRINTLN(" response!");  // Сообщаем, что получили подтверждение
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
bool checkNetwork() {
    String response; // Переменная для хранения ответа модема
    int attempts = 5; // Количество попыток проверки регистрации
    bool registered = false; // Флаг успешной регистрации

    // Проверка регистрации в сети с несколькими попытками
    for (int i = 0; i < attempts; i++) {
        DEBUG_PRINT("Попытка регистрации: ");
        DEBUG_PRINTLN(i + 1);

        // Отправляем команду AT+CREG?
        gsmSerial.println("AT+CREG?");
        delay(500); // Даем модему время на ответ

        // Чтение ответа от модема
        response = "";
        unsigned long startTime = millis();
        while (millis() - startTime < 1000) { // Ждем ответа 1 секунду
            if (gsmSerial.available()) {
                response += gsmSerial.readString();
            }
        }

        // DEBUG_PRINTLN("Ответ модема: " + response); // Вывод ответа для отладки

        // Проверка, зарегистрирован ли модем в сети
        if (response.indexOf("+CREG: 0,1") != -1 || response.indexOf("+CREG: 0,5") != -1) {
            DEBUG_PRINTLN("Модем зарегистрирован в сети");
            registered = true; // Устанавливаем флаг успешной регистрации
            break; // Выходим из цикла
        }

        // DEBUG_PRINTLN("Модем не зарегистрирован в сети, повторная попытка...");
        delay(1000); // Задержка перед следующей попыткой
    }

    // Если регистрация не удалась
    if (!registered) {
        DEBUG_PRINTLN("Модем не зарегистрирован в сети после всех попыток");
        return false;
    }

    // Проверка уровня сигнала
    gsmSerial.println("AT+CSQ");
    delay(500); // Даем модему время на ответ

    // Чтение ответа от модема
    response = "";
    unsigned long startTime = millis();
    while (millis() - startTime < 1000) { // Ждем ответа 1 секунду
        if (gsmSerial.available()) {
            response += gsmSerial.readString();
        }
    }

    // DEBUG_PRINTLN("Ответ модема: " + response); // Вывод ответа для отладки

    // Извлечение уровня сигнала
    int csq = -1;
    int index = response.indexOf("+CSQ: ");
    if (index != -1) {
        csq = response.substring(index + 6, index + 8).toInt(); // Извлекаем значение уровня сигнала
    }

    DEBUG_PRINT("Уровень сигнала: ");
    DEBUG_PRINTLN(csq);

    // Проверка уровня сигнала (для SIM800 допустимые значения 0-31, где 99 - ошибка)
    if (csq == 99 || csq < 10) {
        DEBUG_PRINTLN("Ошибка: слабый сигнал или нет сети!");
        return false;
    }

    DEBUG_PRINTLN("Уровень сигнала в норме");
    return true;
}

bool initModem() {
    gsmSerial.begin(9600);
    delay(2000);

#ifdef USE_M590
    DEBUG_PRINTLN("Ожидание готовности модема...");

    while (!gsmSerial.find("PBREADY")) {  // Ждём сообщение "+PBREADY"
        indicateLed(1, 500, 500); // мигаем с частотой 0.5 сек
    }

    // DEBUG_PRINTLN("Модем готов!");

    // Настройка модема
    if (sendAtCmd("AT")) DEBUG_PRINTLN("Модем отвечает"); // Проверка связи с модемом
    else if (sendAtCmd("AT+IPR=9600")) DEBUG_PRINTLN("Скорость 9600 задана"); delay(1000);  // команда модему на установку скорости
    if (sendAtCmd("AT+CLIP=1")) DEBUG_PRINTLN("АОН включен");                               // включаем АОН
    if (sendAtCmd("AT+CMGF=1")) DEBUG_PRINTLN("Режим SMS установлен");                      // режим кодировки СМС - обычный (для англ.)
    if (sendAtCmd("AT+CSCS=\"GSM\"")) DEBUG_PRINTLN("Кодировка текста установлена");        // режим кодировки текста
    if (sendAtCmd("AT+CNMI=2,2")) DEBUG_PRINTLN("Настройки отображения SMS установлены");   // отображение смс в терминале сразу после приема (без этого сообщения молча падают в память)
    if (sendAtCmd("AT&W")) DEBUG_PRINTLN("Настройки сохранены");                            // сохранение настроек в энергонезависимой памяти
    delay(500);

    if (!checkNetwork()) return false;        // Проверка регистрации в сети и уровня сигнала

    // Очистка памяти SMS
    if (sendAtCmd("AT+CMGD=1,4")) DEBUG_PRINTLN("Память модема очищена"); // Удаление всех SMS

    DEBUG_PRINTLN("\nМодем успешно инициализирован");
    return true;
#endif

#ifdef USE_SIM800
    DEBUG_PRINTLN("Ожидание готовности модема...");

    // Ждём сообщение "RDY" или "SMS Ready" от модема
    while (!gsmSerial.find("RDY") && !gsmSerial.find("Ready")) {
        indicateLed(1, 500, 500); // мигаем с частотой 0.5 сек
    }

    DEBUG_PRINTLN("Модем готов!");

    // Настройка модема
    if (sendAtCmd("AT")) DEBUG_PRINTLN("Модем отвечает"); // Проверка связи с модемом
    else if (sendAtCmd("AT+IPR=9600")) DEBUG_PRINTLN("Скорость 9600 задана"); delay(1000);      // команда модему на установку скорости
    if (sendAtCmd("AT+CMGF=1")) DEBUG_PRINTLN("Режим SMS установлен");                           // режим кодировки СМС - обычный (для англ.)
    if (sendAtCmd("AT+CNMI=1,2,0,0,0")) DEBUG_PRINTLN("Настройки отображения SMS установлены"); // Настройка приема SMS
    if (sendAtCmd("AT+CSMP=17,167,0,0")) DEBUG_PRINTLN("Кодировка текста установлена");         // Настройка кодировки SMS

    if (!checkNetwork()) return false;        // Проверка регистрации в сети и уровня сигнала

#endif
}

//--------------------------------------------------------------
// Обработчик прерывания кнопки с защитой от дребезга
//--------------------------------------------------------------
void buttonISR() {
  btnFlag=true;
  lastPressTime = millis();
}

//--------------------------------------------------------------
// Обработчик прерывания при поступлении смс или звонка
//--------------------------------------------------------------
void ringISR() {
    ringFlag = true;
}

//---------------------------------------------------
// Процедура отправки СМС
//---------------------------------------------------
void sendSMS(String text, String phone)
{
  gsmSerial.println("AT+CMGS=\"+" + phone + "\"");
  delay(500);
  gsmSerial.print(text);
  delay(500);
  gsmSerial.print((char)26);
  delay(5000);
}

//--------------------------------------------------------------
// Чтение номера из СИМ
//--------------------------------------------------------------
#ifdef USE_READ_NUM_SIM
void readNumberSIM() {
  byte ch = 0;
  byte x = 0;
  while (gsmSerial.available()) gsmSerial.read();
  delay(100);
  gsmSerial.println("AT+CPBF=\"oneNum\"");     //чтение номера из СИМ
  delay(300);
  while (gsmSerial.available()) {         //сохраняем входную строку в переменную val
    ch = gsmSerial.read();
    x++;
    if((x > 30) && (x < 42)) {
      val += char(ch);
      delay(5);
    }
   }
   if (val.indexOf("79") > -1 && val.length() == 11) {
     oneNum = val;
     update_eeprom_number(10, oneNum);
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

    for (int i = 0; i < 12; i++) {  // Читаем до 11 символов
        ch = EEPROM.read(addr + i);
        if (ch == '\0' || ch == 0xFF) break;  // Останавливаемся, если пусто
        num += ch;
    }

    // Проверяем, корректен ли номер
    if (num.length() == 11 && num.startsWith("79")) {
        return num;  // Если номер правильный, возвращаем его
    }
    // if (addr==10) DEBUG_PRINTLN("\n1-й номер из EEPROM не считан");
    // else DEBUG_PRINTLN("\n2-й номер из EEPROM не считан");
}

//--------------------------------------------------------------
// Запись номера в EEPROM
//--------------------------------------------------------------
void update_eeprom_number(int addr, String num) {
  for (byte i = 0; i < 12; i++) EEPROM.write(addr + i, num[i]);
}

//--------------------------------------------------------------
// Управление нагрузкой
//--------------------------------------------------------------
void switchPower(bool newState) {
  digitalWrite(POWER_PIN, newState);
  digitalWrite(STATE_LED, newState);
  state = newState;
  if (saveState) {
    EEPROM.update(STATE_ADDR, newState);
  }
  // DEBUG_PRINTLN("\nPOWER: " + String(state ? "ON" : "OFF"));  // Сообщаем, что получили подтверждение
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
    if (timer == 0 || millis() >= timer) {
        switchPower(false);
        timer = 0;
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
  pinMode(POWER_PIN, OUTPUT);
  pinMode(STATE_LED, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(RING_PIN, INPUT_PULLUP);
  pinMode(HEATER, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), buttonISR, FALLING); // Настройка аппаратного прерывания
  attachInterrupt(digitalPinToInterrupt(RING_PIN), ringISR, FALLING);

bool initFlag = false;
int attempts = 3;
while (attempts-- > 0) {
    if (initModem()) {
        initFlag = true;
        break;
    }
    #ifdef USE_M590
        if(sendAtCmd("AT+CRESET")) //DEBUG_PRINTLN("Модем перезагружен");
    #endif
    #ifdef USE_SIM800
        if(sendAtCmd("AT+CFUN=1,1")) //DEBUG_PRINTLN("Модем перезагружен");
    #endif
    delay(5000); // Ждем 5 секунд перед повторной попыткой
}
if (!initFlag) while(1) indicateLed(2, 200, 1000); // 2 мигания (0.2 сек) пауза 1 сек

  saveState = EEPROM.read(SS_ADDR);
  if(saveState) {
    state = EEPROM.read(STATE_ADDR);
    switchPower(state);
  }
  replySMS = EEPROM.read(REPL_ADDR);
  #ifdef USE_HEATING
  heaterVal = EEPROM.read(HEAT_ADDR);
  #endif

  oneNum = read_eeprom_number(10);
  twoNum = read_eeprom_number(30);
  // DEBUG_PRINTLN("Мастер номер: " + oneNum + "\nМастер номер 2: " + twoNum);
}

//--------------------------------------------------------------
// Основной цикл loop
//--------------------------------------------------------------
void loop() {
  if(ringFlag) {
    ringFlag = false;               // Сбрасываем флаг
    incoming_call_sms();            // Запускаем функцию обработки смс ил звонка
  }
  if(btnFlag==true && (millis() - lastPressTime > 200)) {   // Антидребизг контактов (ждем 200 мс)
    btnFlag=false;                  // Сбрасываем флаг
    lastPressTime = 0;              // Сбрасываем время пред-го нажатия
    switchPower(!state);            // Запускаем функцию управления нагрузкой
  }
  #ifdef USE_TIMER
  if(timer!=0) timerControl();
  #endif
  #ifdef USE_HEATING
  if(heaterVal<1) heaterControl();
  #endif
}


//-----------------------------------------------------------------------------
// Процедура обработки звонков и смс
//-----------------------------------------------------------------------------

Command getCommand(const String& val) {
    if (val.indexOf("+CMT") > -1) {
        val.trim();                     // Очищаем от пробелов и \n
        val.toLowerCase();              // Приводим к нижнему регистру для единообразия
        if (val.indexOf("power on") > -1 && CHECK_NUMBER && !state) return CMD_POWER_ON;
        if (val.indexOf("power off") > -1 && CHECK_NUMBER && state) return CMD_POWER_OFF;
        if (val.indexOf("timer") > -1 && CHECK_NUMBER) return CMD_TIMER;
        if (val.indexOf("heating off") > -1 && CHECK_NUMBER) return CMD_HEATING_OFF;
        if (val.indexOf("heating ") > -1 && CHECK_NUMBER) return CMD_HEATING;
        if (val.indexOf("save state") > -1 && CHECK_NUMBER) return CMD_SAVE_STATE;
        if (val.indexOf("reply sms") > -1 && CHECK_NUMBER) return CMD_REPLY_SMS;
        if (val.indexOf("temper") > -1 && CHECK_NUMBER) return CMD_TEMPERATURE;
        if (val.indexOf("status") > -1 && CHECK_NUMBER) return CMD_STATUS;
        if (val.indexOf("clear") > -1 && CHECK_NUMBER) return CMD_CLEAR;
        if (val.indexOf("new one number") > -1) return CMD_ONE_NUM;
        if (val.indexOf("new two number") > -1) return CMD_TWO_NUM;
        if (val.indexOf("new sim number") > -1) return CMD_SIM_NUM;
    }
    if (val.indexOf("RING") > -1) return CMD_RING;
    // DEBUG_PRINTLN("Команда не найдена, возвращаем CMD_UNKNOWN.");
    return CMD_UNKNOWN;
}

//--------------------------------------------------------------
// Функция извлечения времени из СМС
//--------------------------------------------------------------
unsigned long extractTime(const String& val) {
    // Удаляем все пробелы из строки
    String cleanedVal = val;
    cleanedVal.replace(" ", "");

    // Находим позицию ключевого слова "timer"
    int timerPos = cleanedVal.indexOf("timer");
    if (timerPos == -1) {
        return 0; // Если ключевое слово "timer" не найдено, возвращаем 0
    }

    // Извлекаем подстроку, начиная с позиции после "timer"
    String timeStr = cleanedVal.substring(timerPos + 5);

    unsigned long hours = 0;
    unsigned long minutes = 0;

    // Ищем символ 'h' для извлечения часов
    int hPos = timeStr.indexOf('h');
    if (hPos != -1) {
        hours = timeStr.substring(0, hPos).toInt();
        timeStr = timeStr.substring(hPos + 1); // Убираем часть строки с часами
    }

    // Ищем символ 'm' для извлечения минут
    int mPos = timeStr.indexOf('m');
    if (mPos != -1) {
        minutes = timeStr.substring(0, mPos).toInt();
    } else if (timeStr.length() > 0) {
        // Если символ 'm' отсутствует, но строка не пустая, значит это минуты
        minutes = timeStr.toInt();
    }

    // Возвращаем общее время в минутах
    return hours * 60 + minutes;
}
//--------------------------------------------------------------
// Функция извлечения номера из СМС
//--------------------------------------------------------------
String extractNumber(String& val) {
    // Ищем ключевое слово "number "
    int numPos = val.indexOf("number ");
    if (numPos > -1) {
        numPos += 7; // Пропускаем слово "number "
        int endNum = numPos;

        // Ищем конец номера (до первого пробела или конца строки)
        while (endNum < val.length() && isDigit(val[endNum])) {
            endNum++;
        }

        // Проверяем, что номер состоит из 11 цифр
        if (endNum - numPos == 11) {
            return val.substring(numPos, endNum); // Возвращаем найденный номер
        }
    }

    // Если номер не найден в сообщении или он некорректен, ищем в заголовке
    int startQuote = val.indexOf("\"") + 1;
    int endQuote = val.indexOf("\"", startQuote);
    if (startQuote > 0 && endQuote > startQuote) {
        String headerNumber = (val[startQuote] == '+') ? val.substring(startQuote + 1, endQuote) : val.substring(startQuote, endQuote);
        if (headerNumber.length() == 11) {
            return headerNumber;
        }
    }

    // return ""; // Если номер не найден или невалиден, возвращаем пустую строку
}

void incoming_call_sms() {
    val = "";  // Очищаем перед приёмом
    byte ch = 0;
    unsigned long startTime = millis();  // Засекаем время начала чтения

    // Ждём прихода данных в буфер (до 3 секунд)
    while (!gsmSerial.available() && millis() - startTime < 3000);

    // Читаем данные
    startTime = millis();  // Перезапускаем таймер
    while (millis() - startTime < 1000) { // Читаем данные 1 секунду
        while (gsmSerial.available()) {
            ch = gsmSerial.read();
            val += char(ch);
            startTime = millis();  // Сброс таймера при поступлении данных
        }
    }
    // DEBUG_PRINT("Получено: ");  // Отладка
    // DEBUG_PRINTLN(val);  // Отладка

    Command cmd = getCommand(val);
    // DEBUG_PRINTLN("Команда: " + String(cmd));  // Должно вывести CMD_...
    switch (cmd) {
        case CMD_STATUS:
            sendSMS("POWER: " + String(state ? "ON" : "OFF") + LINE_BREAK +
                    "SAVE STATE POWER: " + String(saveState ? "ON" : "OFF") + LINE_BREAK +
                    "TEMP: " + String(currentTemper()) + "'C" + LINE_BREAK +
                    "REPLY SMS: " + String(replySMS ? "ON" : "OFF") + LINE_BREAK +
                    "NUM1: " + oneNum + LINE_BREAK +
                    "NUM2: " + twoNum, NUMBER_TO_SEND);
            break;
        case CMD_TEMPERATURE:
            sendSMS("Temperature: " + String(currentTemper()) + "'C", NUMBER_TO_SEND);
            break;
        case CMD_POWER_ON:
            switchPower(true);
            sendSMS("POWER: " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
            break;
        case CMD_POWER_OFF:
            switchPower(false);
            sendSMS("POWER: " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
            break;
        case CMD_CLEAR:
            sendSMS("CLEAR OK", NUMBER_TO_SEND);
            delay(1000);
            sendAtCmd("AT+CMGD=1,4");
            // DEBUG_PRINTLN("Память очищена");
            break;
        case CMD_REPLY_SMS:
            replySMS = !replySMS;
            EEPROM.update(REPL_ADDR, replySMS);
            sendSMS("REPLY SMS: " + String(replySMS ? "ON" : "OFF"), NUMBER_TO_SEND);
            // DEBUG_PRINTLN("Отправка ответных СМС на звонок: " + String(replySMS ? "ON" : "OFF"));
            break;
        case CMD_SAVE_STATE:
            saveState = !saveState;
            EEPROM.update(SS_ADDR, saveState);
            sendSMS("SAVE STATE POWER: " + String(saveState ? "ON" : "OFF"), NUMBER_TO_SEND);
            // DEBUG_PRINTLN("Сохр. состояния нагрузки: " + String(saveState ? "ON" : "OFF"));
            break;
        case CMD_ONE_NUM:
            oneNum = extractNumber(val);
            update_eeprom_number(10,oneNum);
            sendSMS("Osnovnoi nomer izmenen na: " + oneNum, NUMBER_TO_SEND);
            DEBUG_PRINTLN("Основной номер изменен на: " + oneNum);
            break;
        case CMD_TWO_NUM:
            twoNum = extractNumber(val);
            update_eeprom_number(30,twoNum);
            sendSMS("Vtoroi nomer izmenen na: " + twoNum, NUMBER_TO_SEND);
            DEBUG_PRINTLN("Второй номер изменен на: " + twoNum);
            break;
#ifdef USE_READ_NUM_SIM
        case CMD_SIM_NUM:
            readNumberSIM();
            sendSMS("Osnovnoi nomer izmenen na: " + oneNum, NUMBER_TO_SEND);
            DEBUG_PRINTLN("Основной номер изменен на: " + oneNum);
            break;
#endif
#ifdef USE_TIMER
        case CMD_TIMER:
            {
                int extractedTime = extractTime(val);
                if (extractedTime > 0) {
                    timer = extractedTime * 60000 + millis(); // Минуты в миллисекунды
                    switchPower(true);
                    sendSMS("TIMER: " + String(extractedTime) + " MIN", NUMBER_TO_SEND);
                } else {
                    sendSMS("TIMER: OFF", NUMBER_TO_SEND);
                }
            }
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
                    // DEBUG_PRINTLN("Подогрев вкл. при темп. " + String(heaterVal) + "'C");
                }
                else {
                    sendSMS("HEATING OFF, TEMP > +1 C`", NUMBER_TO_SEND);
                    // DEBUG_PRINTLN("Подогрев выкл. т.к. темп. выше +1`C");
                }
            }
            break;
        case CMD_HEATING_OFF:
            heaterVal = 1;
            EEPROM.update(HEAT_ADDR, heaterVal);
            sendSMS("HEATING OFF OK", NUMBER_TO_SEND);
            // DEBUG_PRINTLN("Подогрев успешно отключен");
            break;
#endif
        case CMD_RING:
            if CHECK_NUMBER {
                delay(500);
                switchPower(!state);
                sendAtCmd("ATH0");       // Завершение вызова
                if (replySMS) {
                    sendSMS("POWER: " + String(state ? "ON" : "OFF"), NUMBER_TO_SEND);
                }
            } else {
                sendAtCmd("ATA");       // Отвечаем на входящий вызов
                delay(7000);            // Ждем 7 сек
                sendAtCmd("ATH0");      // Завершение вызова
            }
            break;
        default:
            DEBUG_PRINTLN("Неизвестная команда");
            break;
    }
    val = "";
}
