/*
Проект GSM реле на Arduino с использованием модуля NEOWAY M590
Проект был написан давно, но небыл никуда опубликован, лежал на ПК

Функционал реле следующий:  
1. Реле вкл и выкл нагрузку по звонку, при звонке на устройство проискодит сброс вызова и смена состояния реле ВКЛ/ВЫКЛ;  
2. Включение и выключение реле через SMS, отправляем смс на устройство с текстом "relay on" "relay off" в ответ получаем смс о статусе;  
3. Включение с таймером через SMS, отправляем смс на устройство с текстом "timer МИН" например "timer 60" включит реле и через 60 мин сам выкл его;  
4. Проверка температуры с датчика DS18b20, отправляем смс на устройство с текстом "temper" в ответ получаем смс с текущей температурой;  
5. Также я попытался реализовать возможность самоподогрева устройства отправкой SMS с текстом "termostat ТЕМПЕРАТУРА ВКЛ ПОДОГРЕВА"
например "termostat -10" если датчик стоит внутри устройства, то при достижении -10 град. вкл самоподогрев;  
6. И самое главное в устройстве реализована защита от сторонних звонков и СМС, в устройстве сохраняются 2 номера тел MASTER и MASTER2 с которых можно управлять устройством.
Имеется возможность смены этих номеров при помощи СМС, для этого отправляем смс с текстом "new master" или "new master2" с номера который вы хотите сделать мастер номером,
то есть если вы хотите номер 123456789 сделать первым, то отправляем с него текст "new master" а если хотите сделать его вторым, то отправляете с него смс "new master2";
*/

#include <EEPROM.h>
#include <SoftwareSerial.h>
SoftwareSerial mySerial(2, 3);         // RX, TX
#include <OneWire.h>

// #define USE_readNumberSIM           // раскоментировать если нужна возможность чтения номера из СИМ карты
// #define USE_TERMOSTAT               // закоментировать если не нужен термостат
#define USE_TIMER                      // закоментировать если не нужен таймер

//---------КОНТАКТЫ--------------
#define power 12                       // пин реле
#define powLED 6                       // индикация режима состояния модема M590
#define LED 7                          // индикация состояния реле
#define BUTTON 4                       // кнопка вкл/выкл розетки вручную
#define heater 13                      // нагреватель (можно использовать керамический ресистор на 3-6к)
#define ds18b20 13                      // датчик температуры DS18b20
OneWire sensDs (ds18b20);                    // датчик подключен к выводу 9
#define DS_POWER_MODE  1                  // режим питания, 0 - внешнее, 1 - паразитное
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
int8_t heaterVal = EEPROM.read(3);
#endif

//--------------------------------------------------------------
// процедура отправки команд модему:
//------------------------------------------------------------------------------------------------------------
bool sendAtCmd(String at_send, String ok_answer = "OK", String err_answer = "", uint16_t wait_sec = 2) {
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

//-------------------------------------------------------------------------------------------
void setup()
{
  //-----------------------------------------------------------------------------------------
  pinMode(11, OUTPUT);
  digitalWrite(11, HIGH);
  pinMode(powLED, OUTPUT);
  digitalWrite(powLED, LOW);
  pinMode(power, OUTPUT);                 // пин 12 подключаем как выход для питания базы транзистора
  if(EEPROM.read(2)) {
    state = EEPROM.read(1);
    digitalWrite(power, state);
  }
  else {
    EEPROM.update(1,state);
    EEPROM.update(2,1);
  }
  pinMode(LED, OUTPUT);
  digitalWrite(LED, state);
  pinMode(heater, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);               //конфигурация кнопки на вход
  
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
      digitalWrite(powLED,!digitalRead(powLED));
    }
    else {
      break;
    }
  }
  sendAtCmd("AT+CMGD=1,4");     //стереть все старые сообщения
  for(byte i=0; i<7; i++) { // Помигаем и подождем перед считыванием номера с СИМ
    digitalWrite(powLED,!digitalRead(powLED));
    delay(150);
    }
  if (read_master_eeprom(10).indexOf("79") > -1 && read_master_eeprom(10).length() == 11) {
    MASTER = read_master_eeprom(10);
  }
  if (read_master2_eeprom(30).indexOf("79") > -1 && read_master2_eeprom(30).length() == 11) {
    MASTER2 = read_master2_eeprom(30);
  }
  
  digitalWrite(powLED,HIGH);
  
  //Serial.print("MASTER №: ");
  //Serial.println(MASTER);
  //Serial.print("MASTER №2: ");
  //Serial.println(MASTER2);
}


// процедура отправки СМС
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

//функция вкл/выкл кнопкой
//----------------------------------------------------
void btnCheck() {
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
   digitalWrite(LED, state);
   EEPROM.update(1,state);
   timer = 0;
   EEPROM.update(2,1);
   while(digitalRead(BUTTON)==LOW) delay(400);
  }
}


//------- функция чтения номера из СИМ -----------
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

//******************************************************************************************
// чтение номера из EEPROM
//******************************************************************************************
String read_master_eeprom( int adr) {
    String number = "";
    for ( byte i = 0; i < 12; i++) EEPROM.get( adr + i, number[i] );
    return number;
} 
String read_master2_eeprom( int adr) {
    String number = "";
    for ( byte i = 0; i < 12; i++) EEPROM.get( adr + i, number[i] );
    return number;
}
//******************************************************************************************
// запись номера в EEPROM
//******************************************************************************************
void update_master_eeprom(int addr) {
    for(byte i = 0; i < 12; i++) EEPROM.put(addr+i, MASTER[i]);
}
void update_master2_eeprom(int addr) {
    for(byte i = 0; i < 12; i++) EEPROM.put(addr+i, MASTER2[i]);
}

float currentTemper() {
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


void loop()
{
  if(digitalRead(BUTTON)==LOW) btnCheck();          //есть была нажата кнопка
  
  if (mySerial.available()) incoming_call_sms();          //есть данные от GSM модуля
  
  #ifdef USE_TERMOSTAT
  if(heaterVal != 0 && millis()-curTime>30000) {
    if(currentTemper() <= heaterVal && tmpFlag == false) {
      digitalWrite(heater,HIGH);
      tmpFlag=true;
    }
    else if(currentTemper() >= (heaterVal+4) && tmpFlag == true) {
      digitalWrite(heater,LOW);
      tmpFlag=false;
      }
    curTime=millis();
    //Serial.println(currentTemper());
  }
  #endif

  #ifdef USE_TIMER
  if(timer != 0) {
    if(timer <= millis()) {
      digitalWrite(power, LOW);
      state = false;
      digitalWrite(LED, state);
      EEPROM.update(1,state);
      timer = 0;
      EEPROM.update(2,1);
     }
  }
  #endif
}


//-----------------------------------------------------------------------------
//процедура обработки звонка и смс
//-----------------------------------------------------------------------------
void incoming_call_sms()
{
  byte ch = 0;
  delay(200);
  while (mySerial.available()) {         //сохраняем входную строку в переменную val
    ch = mySerial.read();
    val += char(ch);
    delay(10);
  }
  //----------------------- определение факта приема СМС и сравнение номера с заданным
  if (val.indexOf("+CMT") > -1)           //если обнаружен СМС (для определения звонка вместо "+CMT" вписать "RING", трубку он не берет, но реагировать на факт звонка можно)
  {

    // для очистки памяти от старых смс-ок, очистка есть при перезапуске, но скидывать клему не хочется
    if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("delete sms") > -1) //если СМС от хозяина и содержит текст запроса
    {
      if (val.indexOf(MASTER) > -1) sms("Delete SMS OK", MASTER); // отвечаем смской
      else sms("Delete SMS OK", MASTER2); // отвечаем смской
      val = "";
      delay(1000);
      sendAtCmd("AT+CMGD=1,4");
    }
    else if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay on") > -1 && state == false) //если СМС от хозяина и содержит текст запроса
    {
      digitalWrite(power, HIGH);
      if (val.indexOf(MASTER) > -1) sms("RELAY ON OK", MASTER); // отвечаем смской
      else sms("RELAY ON OK", MASTER2); // отвечаем смской
      val = "";
      state = true;
      digitalWrite(LED, state);
      EEPROM.update(1,state);
      timer = 0;
      EEPROM.update(2,1);
    }
    else if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("relay off") > -1 && state == true) //если СМС от хозяина и содержит текст запроса
    {
      digitalWrite(power, LOW);
      if (val.indexOf(MASTER) > -1) sms("RELAY OFF OK", MASTER); // отвечаем смской
      else sms("RELAY OFF OK", MASTER2); // отвечаем смской
      val = "";
      state = false;
      digitalWrite(LED, state);
      EEPROM.update(1,state);
      timer = 0;
      EEPROM.update(2,1);
    }
    
    #ifdef USE_TIMER
    else if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("timer ") > -1)
      {
      String timerTmp = val.substring(54);
      timer = timerTmp.toInt();
      //Serial.println(timer);
      timerTmp = (String)timer;
      timer = timer*60*1000+millis();
      if(timer != 0) {
        digitalWrite(power, HIGH);
        state = true;
        digitalWrite(LED, state);
        EEPROM.update(1,state);
        EEPROM.update(2,0);
        if (val.indexOf(MASTER) > -1) sms("TIMER ON " + timerTmp + " MIN", MASTER); // отвечаем смской
        else sms("TIMER ON " + timerTmp + " MIN", MASTER2); // отвечаем смской
        }
        val = "";
      }
    #endif

    else if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("temper") > -1) //если СМС от хозяина и содержит текст запроса температуры
    {
      String message = (String)currentTemper();
      if (val.indexOf(MASTER) > -1) sms("Temperature: " + message + "'C", MASTER); // в СМС отправляем текущую температуру
      else sms("Temperature: " + message + "'C", MASTER2); // в СМС отправляем текущую температуру
      val = "";
    }

    #ifdef USE_TERMOSTAT
    else if ((val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1) && val.indexOf("termostat ") > -1)
      {
      String heatTmp = val.substring(58);
      heaterVal = heatTmp.toInt();
      //Serial.println(heaterVal);
      EEPROM.update(3,heaterVal);
      if(heaterVal != 0) {
        String message = (String)heaterVal; // Массив для вывода
        if (val.indexOf(MASTER) > -1) sms("TERMOSTAT ON " + message + "'C", MASTER); // отвечаем смской
        else sms("TERMOSTAT ON " + message + "'C", MASTER2); // отвечаем смской
        }
        val = "";
      }
    #endif
      
    else if (val.indexOf("new master") > -1)
      {
      MASTER = val.substring(10, 21);
      //Serial.println(MASTER);
      update_master_eeprom(10);
      sms("Master Nomer izmenen", MASTER); // отвечаем смской
      val = "";
      }

    else if (val.indexOf("new master2") > -1)
      {
      MASTER2 = val.substring(10, 21);
      //Serial.println(MASTER2);
      update_master2_eeprom(30);
      sms("Master Nomer izmenen", MASTER2); // отвечаем смской
      val = "";
      }

    #ifdef USE_readNumberSIM
    else if (val.indexOf("SIM master N") > -1)
      {
      val="";
      readNumberSIM();
      //Serial.println(MASTER);
      sms("Master Nomer izmenen", MASTER); // отвечаем смской
      }
      #endif
      
    else
    {
      val = "";
    }
  }
  else if (val.indexOf("RING") > -1)           //если обнаружен звонок
  {
    {
      if (val.indexOf(MASTER) > -1 || val.indexOf(MASTER2) > -1)         //если звонок от хозяина

      {
        delay(500); // выждем, чтоб модем перешел в режим ожидания, иначе просадка напряжения возможна и перезагрузка
        if (state == LOW)
        {
          digitalWrite(power, HIGH);
          //delay(100);
          //char message[15] = "RELAY ON OK"; // Массив для вывода
          //sms(message, MASTER); // отвечаем смской
          val = "";
          delay(400);
          state = true;
          digitalWrite(LED, state);
          EEPROM.update(1,state);
          timer = 0;
          EEPROM.update(2,1);
        }
        else if (state == HIGH)
        {
          digitalWrite(power, LOW);
          //delay(100);
          //char message[15] = "RELAY OFF OK"; // Массив для вывода
          //sms(message, MASTER); // отвечаем смской
          val = "";
          delay(400);
          state = false;
          digitalWrite(LED, state);
          EEPROM.update(1,state);
          timer = 0;
          EEPROM.update(2,1);
        }
        sendAtCmd("ATH0");     // сбросить вызов

      }
      else
      {
        delay(10500);
        sendAtCmd("ATH");
      }
    }
  }
  val = "";
}



