//******************************************************************************************
// Проект GSM реле на Arduino с использованием модуля NEOWAY M590
//******************************************************************************************


#include <EEPROM.h>
#include <SoftwareSerial.h>
SoftwareSerial mySerial(A2, A3);         // RX, TX
#include <OneWire.h>

// #define USE_readNumberSIM           // раскоментировать для считывания номера из СИМ карты
// #define USE_TERMOSTAT               // раскоментировать для включения самоподогрева
#define USE_TIMER                      // закоментировать если не нужен таймер

//---------КОНТАКТЫ--------------
#define power 12                       // пин реле
#define STAT_LED 13                     // индикация состояния реле и состояния модема M590 при включении
#define BUTTON 4                       // кнопка вкл/выкл розетки вручную
#define HEATER 6                      // нагреватель (можно использовать керамический ресистор на 3-6к)
#define DS18B20 7                     // датчик температуры DS18B20
#define DS_POWER_MODE  1               // режим питания, 0 - внешнее, 1 - паразитное
OneWire sensDs (DS18B20);              // датчик подключен к выводу 9
//---------------------------------
String MASTER = "79123456789";          // 1-й телефон владельца
String MASTER2 = "79123456789";         // 2-й телефон владельца
String val = "";
boolean state = false;                  // состояние (вкл/выкл)
#ifdef USE_TIMER
uint32_t timer = 0;
uint32_t curTime;
#endif
byte bufData[9];                        // буфер данных для считывания температуры с DS18B20
#ifdef USE_TERMOSTAT
byte tmpFlag;
int8_t HEATERVal = EEPROM.read(3);
#endif

//--------------------------------------------------------------
// процедура отправки команд модему:
//------------------------------------------------------------------------------------------------------------
bool sendAtCmd(String at_send, String ok_answer = "OK", String err_answer = "", uint16_t wait_sec = 2)
{
  uint32_t exit_ms = millis() + wait_sec * 1000;
  String answer;
  if (at_send != "") mySerial.println(at_send);
  while (millis() < exit_ms) {
    if (mySerial.available()) {
      answer = mySerial.readString();
      if (err_answer != "" && (answer.indexOf(err_answer) > -1 || err_answer.indexOf(answer))) {
        return false;
      }
      else if (answer.indexOf(ok_answer) > -1) {
        return true;
      }
    }
  }

  return false;
}


//-----------------------------------------------------------------------------------------
//                             ИНИЦИАЛИЗАЦИЯ УСТРОЙСТВА
//-----------------------------------------------------------------------------------------
void setup()
{
  pinMode(11, OUTPUT);
  digitalWrite(11, HIGH);
  pinMode(STAT_LED, OUTPUT);
  digitalWrite(STAT_LED, LOW);
  pinMode(power, OUTPUT);                 // пин 12 подключаем как выход для питания базы транзистора
  if(EEPROM.read(2)) {
    state = EEPROM.read(1);
    digitalWrite(power, state);
  }
  else {
    EEPROM.update(1,state);
    EEPROM.update(2,1);
  }
  pinMode(STAT_LED, OUTPUT);
  digitalWrite(STAT_LED, state);
  pinMode(HEATER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);               // конфигурация кнопки на вход

  //Serial.begin(9600);
  mySerial.begin(9600);                //подключаем порт модема
  sendAtCmd("AT+IPR?");                // запрос у модема скорости
  delay(100);
  if(!mySerial.find("+IPR=9600")) {
    sendAtCmd("AT+IPR=9600");            // команда модему на установку скорости
    delay(50);
    sendAtCmd("AT&W");
    delay(200);
  }
  mySerial.println("AT+CLIP=1");      //включаем АОН
  delay(300);
  mySerial.println("AT+CMGF=1");       //режим кодировки СМС - обычный (для англ.)
  delay(300);
  mySerial.println("AT+CSCS=\"GSM\""); //режим кодировки текста
  delay(300);
  mySerial.println("AT+CNMI=2,2");     //отображение смс в терминале сразу после приема (без этого сообщения молча падают в память)
  delay(300);

  while (!mySerial.find("+PBREADY")) {
    mySerial.println("AT+CSQ");          //вывести в терминал уровень сигнала (если 99, то связи нет)
    delay(300);
    if(mySerial.find("+CSQ: 99")) {
      digitalWrite(STAT_LED,!digitalRead(STAT_LED));
    }
    else {
      break;
    }
  }
  sendAtCmd("AT+CMGD=1,4");     //стереть все старые сообщения
  for(byte i=0; i<7; i++) { // Помигаем и подождем перед считыванием номера с СИМ
    digitalWrite(STAT_LED,!digitalRead(STAT_LED));
    delay(150);
    }
  if (read_master_eeprom(10).indexOf("79") > -1 && read_master_eeprom(10).length() == 11) {
    MASTER = read_master_eeprom(10);
  }
  if (read_master2_eeprom(30).indexOf("79") > -1 && read_master2_eeprom(30).length() == 11) {
    MASTER2 = read_master2_eeprom(30);
  }

  digitalWrite(STAT_LED,HIGH);

  //Serial.print("MASTER №: ");
  //Serial.println(MASTER);
  //Serial.print("MASTER №2: ");
  //Serial.println(MASTER2);
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


//---------------------------------------------------
// Процедура обработки кнопки
//---------------------------------------------------
void btnCheck()
{
  delay(40);
  if(digitalRead(BUTTON)==LOW) {
   if (state == false) {
    digitalWrite(power, HIGH);
    state = true;
   }
   else {
    digitalWrite(power, LOW);
    state = false;
   }
   digitalWrite(STAT_LED, state);
   EEPROM.update(1,state);
   timer = 0;
   EEPROM.update(2,1);
   while(digitalRead(BUTTON)==LOW) delay(400);
  }
}


//---------------------------------------------------
// Процедура чтения номера из СИМ
//---------------------------------------------------
#ifdef USE_readNumberSIM
void readNumberSIM()
{
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


//---------------------------------------------------
// Процедура чтения номера из EEPROM
//---------------------------------------------------
String read_master_eeprom( int adr)
{
    String number = "";
    for ( byte i = 0; i < 12; i++) EEPROM.get( adr + i, number[i] );
    return number;
}
String read_master2_eeprom( int adr) {
    String number = "";
    for ( byte i = 0; i < 12; i++) EEPROM.get( adr + i, number[i] );
    return number;
}


//---------------------------------------------------
// Процедура записи номера в EEPROM
//---------------------------------------------------
void update_master_eeprom(int addr)
{
    for(byte i = 0; i < 12; i++) EEPROM.put(addr+i, MASTER[i]);
}
void update_master2_eeprom(int addr)
{
    for(byte i = 0; i < 12; i++) EEPROM.put(addr+i, MASTER2[i]);
}


//---------------------------------------------------
// Процедура измерения температуры с датчика DS18B20
//---------------------------------------------------
float currentTemper()
{
    float temperature;  // измеренная температура
    sensDs.reset();  // сброс шины
    sensDs.write(0xCC, DS_POWER_MODE); // пропуск ROM
    sensDs.write(0x44, DS_POWER_MODE); // инициализация измерения
    delay(900);  // пауза 0,9 сек
    sensDs.reset();  // сброс шины
    sensDs.write(0xCC, DS_POWER_MODE); // пропуск ROM
    sensDs.write(0xBE, DS_POWER_MODE); // команда чтения памяти датчика
    sensDs.read_bytes(bufData, 9);  // чтение памяти датчика, 9 байтов
    if ( OneWire::crc8(bufData, 8) == bufData[8] ) {  // проверка CRC
      // если данные правильные
      temperature =  (float)((int)bufData[0] | (((int)bufData[1]) << 8)) * 0.0625 + 0.03125;
      return temperature;
    }
}


//-----------------------------------------------------------------------------
// Основной цикл loop
//-----------------------------------------------------------------------------
void loop()
{
  if(digitalRead(BUTTON)==LOW) btnCheck();                // если была нажата кнопка

  if (mySerial.available()) incoming_call_sms();          // если пришли данные от GSM модуля

  #ifdef USE_TERMOSTAT                                    // если включена функция самоподогрева модема
  if(HEATERVal != 0 && millis()-curTime>30000)
  {
    if(currentTemper() <= HEATERVal && tmpFlag == false) {
      digitalWrite(HEATER,HIGH);
      tmpFlag=true;
    }
    else if(currentTemper() >= (HEATERVal+4) && tmpFlag == true) {
      digitalWrite(HEATER,LOW);
      tmpFlag=false;
      }
    curTime=millis();
    //Serial.println(currentTemper());
  }
  #endif

  #ifdef USE_TIMER                                          // если включена функция таймера
  if(timer != 0)
  {
    if(timer <= millis()) {
      digitalWrite(power, LOW);
      state = false;
      digitalWrite(STAT_LED, state);
      EEPROM.update(1,state);
      timer = 0;
      EEPROM.update(2,1);
     }
  }
  #endif
}                   // end loop


//-----------------------------------------------------------------------------
// Процедура обработки звонков и смс
//-----------------------------------------------------------------------------
enum Command {
    CMD_DELETE_SMS,
    CMD_RELAY_ON,
    CMD_RELAY_OFF,
    CMD_TIMER,
    CMD_TEMPERATURE,
    CMD_TERMOSTAT,
    CMD_NEW_MASTER,
    CMD_NEW_MASTER2,
    CMD_SIM_MASTER,
    CMD_RING,
    CMD_UNKNOWN
};

Command getCommand(const String& val) {
    if (val.indexOf("+CMT") > -1) {
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("delete sms") > -1) return CMD_DELETE_SMS;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay on") > -1 && !state) return CMD_RELAY_ON;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay off") > -1 && state) return CMD_RELAY_OFF;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("timer ") > -1) return CMD_TIMER;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("temper") > -1) return CMD_TEMPERATURE;
        if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("termostat ") > -1) return CMD_TERMOSTAT;
        if (val.indexOf("new master") > -1) return CMD_NEW_MASTER;
        if (val.indexOf("new master2") > -1) return CMD_NEW_MASTER2;
        if (val.indexOf("SIM master N") > -1) return CMD_SIM_MASTER;
    }
    if (val.indexOf("RING") > -1) return CMD_RING;
    return CMD_UNKNOWN;
}

void incoming_call_sms()
{
    byte ch = 0;
    delay(200);
    while (mySerial.available()) {
        ch = mySerial.read();
        val += char(ch);
        delay(10);
    }

    Command cmd = getCommand(val);
    switch (cmd) {
        case CMD_DELETE_SMS:
            sms("Delete SMS OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            val = "";
            delay(1000);
            sendAtCmd("AT+CMGD=1,4");
            break;
        case CMD_RELAY_ON:
            digitalWrite(power, HIGH);
            sms("RELAY ON OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            state = true;
            digitalWrite(STAT_LED, state);
            EEPROM.update(1, state);
            timer = 0;
            EEPROM.update(2, 1);
            break;
        case CMD_RELAY_OFF:
            digitalWrite(power, LOW);
            sms("RELAY OFF OK", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            state = false;
            digitalWrite(STAT_LED, state);
            EEPROM.update(1, state);
            timer = 0;
            EEPROM.update(2, 1);
            break;
#ifdef USE_TIMER
        case CMD_TIMER:
            {
                String timerTmp = val.substring(54);
                timer = timerTmp.toInt() * 60000 + millis();
                if (timer != 0) {
                    digitalWrite(power, HIGH);
                    state = true;
                    digitalWrite(STAT_LED, state);
                    EEPROM.update(1, state);
                    EEPROM.update(2, 0);
                    sms("TIMER ON " + timerTmp + " MIN", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                }
                val = "";
            }
            break;
#endif
        case CMD_TEMPERATURE:
            sms("Temperature: " + String(currentTemper()) + "'C", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
            break;
#ifdef USE_TERMOSTAT
        case CMD_TERMOSTAT:
            {
                String heatTmp = val.substring(58);
                HEATERVal = heatTmp.toInt();
                EEPROM.update(3, HEATERVal);
                if (HEATERVal != 0) {
                    sms("TERMOSTAT ON " + String(HEATERVal) + "'C", (val.indexOf(MASTER) > -1) ? MASTER : MASTER2);
                }
                val = "";
            }
            break;
#endif
        case CMD_NEW_MASTER:
            MASTER = val.substring(10, 21);
            update_master_eeprom(10);
            sms("Master Nomer izmenen", MASTER);
            break;
        case CMD_NEW_MASTER2:
            MASTER2 = val.substring(10, 21);
            update_master2_eeprom(30);
            sms("Master Nomer izmenen", MASTER2);
            break;
#ifdef USE_readNumberSIM
        case CMD_SIM_MASTER:
            val = "";
            readNumberSIM();
            sms("Master Nomer izmenen", MASTER);
            break;
#endif
        case CMD_RING:
            if (val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) {
                delay(500);
                state = !state;
                digitalWrite(power, state ? HIGH : LOW);
                digitalWrite(STAT_LED, state);
                EEPROM.update(1, state);
                timer = 0;
                EEPROM.update(2, 1);
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
