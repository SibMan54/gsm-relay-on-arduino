# GSM реле на Arduino  
## Проект GSM реле на Arduino с использованием модуля NEOWAY M590

### Функционал реле следующий:  
1. Реле вкл и выкл нагрузку по звонку, при звонке на устройство проискодит сброс вызова и смена состояния реле ВКЛ/ВЫКЛ;  
2. Включение и выключение реле через SMS, отправляем смс на устройство с текстом "relay on" "relay off" в ответ получаем смс о статусе;  
3. Включение с таймером через SMS, отправляем смс на устройство с текстом "timer МИН" например "timer 60" включит реле и через 60 мин сам выкл его;  
4. Проверка температуры с датчика DS18b20, отправляем смс на устройство с текстом "temper" в ответ получаем смс с текущей температурой;  
5. Я попытался реализовать возможность самоподогрева устройства отправкой SMS с текстом "termostat ТЕМПЕРАТУРА ВКЛ ПОДОГРЕВА"
например "termostat -10" если датчик стоит внутри устройства, то при достижении -10 град. вкл самоподогрев;  
6. Также в устройстве реализована защита от сторонних звонков и СМС, в устройстве сохраняются 2 номера тел MASTER и MASTER2 с которых можно управлять устройством.
Имеется возможность смены этих номеров при помощи СМС, для этого отправляем смс с текстом "new master" или "new master2" с номера который вы хотите сделать мастер номером,
то есть если вы хотите номер 123456789 сделать первым, то отправляем с него текст "new master" а если хотите сделать его вторым, то отправляете с него смс "new master2";
