#include "WiFiModule.h"
#include "ModuleController.h"
#include "InteropStream.h"
//--------------------------------------------------------------------------------------------------------------------------------
#define WIFI_DEBUG_WRITE(s,ca) { Serial.print(String(F("[CA] ")) + String((ca)) + String(F(": ")));  Serial.println((s)); }
#define CHECK_QUEUE_TAIL(v) { if(!actionsQueue.size()) {Serial.println(F("[QUEUE IS EMPTY!]"));} else { if(actionsQueue[actionsQueue.size()-1]!=(v)){Serial.print(F("NOT RIGHT TAIL, WAITING: ")); Serial.print((v)); Serial.print(F(", ACTUAL: "));Serial.println(actionsQueue[actionsQueue.size()-1]); } } }
#define CIPSEND_COMMAND F("AT+CIPSENDBUF=") // F("AT+CIPSEND=")
//--------------------------------------------------------------------------------------------------------------------------------
#if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
void WiFiModule::SendData(IoTService service,uint16_t dataLength, IOT_OnWriteToStream writer, IOT_OnSendDataDone onDone)
{
    // тут смотрим, можем ли мы обработать запрос на отсыл данных в IoT
    IoTSettings iotSettings = MainController->GetSettings()->GetIoTSettings();

    //#if defined(THINGSPEAK_ENABLED) 
    if(iotSettings.Flags.ThingSpeakEnabled && strlen(iotSettings.ThingSpeakChannelID)) // включен один сервис хотя бы
    {

     // сохраняем указатели на функции обратного вызова
      iotWriter = writer;
      iotDone = onDone;
      iotService = service;

      #ifdef WIFI_DEBUG
        Serial.println(F("Requested to write data to IoT using ESP..."));
      #endif

      // Тут формируем данные для запроса
      switch(service)
      {
         case iotThingSpeak:
         {
          // попросили отослать данные через ThingSpeak
          delete iotDataHeader;
          delete iotDataFooter;
          iotDataHeader = new String();
          iotDataFooter = new String();

          // формируем запрос
          *iotDataHeader = F("GET /update?api_key=");
          *iotDataHeader += iotSettings.ThingSpeakChannelID;
          *iotDataHeader += F("&");

          *iotDataFooter = F(" HTTP/1.1\r\nAccept: */*\r\nUser-Agent: ");
          *iotDataFooter += IOT_USER_AGENT;
          *iotDataFooter += F("\r\nHost: ");
          *iotDataFooter += THINGSPEAK_HOST;
          *iotDataFooter += F("\r\n\r\n");

          // теперь вычисляем, сколько всего данных будет
          iotDataLength = iotDataHeader->length() + iotDataFooter->length() + dataLength;

          // теперь можно добавлять в очередь запрос на обработку. Но ситуация с очередью следующая:
          // мы не знаем, чем сейчас занят ESP, и что у нас в очереди. Мы знаем только, что нельзя разбивать
          // команды wfaCIPSEND и wfaACTUALSEND, поскольку после отработки первой в очередь следом ОБЯЗАНА
          // быть помещена вторая, иначе - аллес, т.к. эти команды используются для отсыла ответа на полученную команду.
          // следовательно, всё, что мы можем сделать - это взвести флаг, что как только очередь освободится - мы должны
          // поместить в неё команду на отсыл данных в IoT.
          flags.wantIoTToProcess = true;
         }
         break;
        
      } // switch
    } // enabled
    else
    {
      // ни одного сервиса не включено
    //#else
      // тут ничего не можем отсылать, сразу дёргаем onDone, говоря, что у нас не получилось отослать
      onDone({false,service});
    //#endif
    }
}
#endif   
//--------------------------------------------------------------------------------------------------------------------------------
bool WiFiModule::IsKnownAnswer(const String& line)
{
  return ( line == F("OK") || line == F("ERROR") || line == F("FAIL") || line.endsWith(F("SEND OK")) || line.endsWith(F("SEND FAIL")));
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::ProcessAnswerLine(String& line)
{

   flags.isAnyAnswerReceived = true; 
    
  #ifdef WIFI_DEBUG
     WIFI_DEBUG_WRITE(line,currentAction);
  #endif

   // проверяем, не перезагрузился ли модем
  if(line == F("ready") && currentAction != wfaWantReady) // мы проверяем на ребут только тогда, когда сами его не вызвали
  {
    #ifdef WIFI_DEBUG
      WIFI_DEBUG_WRITE(F("ESP boot found, init queue.."),currentAction);
    #endif


     // убеждаемся, что мы вызвали коллбэк для отсыла данных в IoT
     #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
      EnsureIoTProcessed();
     #endif

     // убеждаемся, что мы обработали HTTP-запрос, пусть и неудачно
     EnsureHTTPProcessed(ERROR_MODEM_NOT_ANSWERING);
     
    InitQueue(false); // инициализировали очередь по новой, т.к. модем либо только загрузился, либо - перезагрузился. При этом мы не добавляем команду перезагрузки в очередь
    needToWaitTimer = WIFI_WAIT_BOOT_TIME; // дадим модему ещё 2 секунды на раздупливание

    return;
  } 

  // тут проверяем, законнекчены ли мы к роутеру или нет
  if(line == F("WIFI DISCONNECT"))
  {
    #ifdef WIFI_DEBUG
      WIFI_DEBUG_WRITE(F("Disconnected from router :("),currentAction);
    #endif
    flags.isConnected = false;
  }
  else
  if(line == F("WIFI CONNECTED"))
  {
    #ifdef WIFI_DEBUG
      WIFI_DEBUG_WRITE(F("Connected to router :)"),currentAction);
    #endif
    flags.isConnected = true;
  }
  

  // здесь может придти ответ от сервера, или - запрос от клиента
  if(
 #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
    currentAction != wfaActualSendIoTData &&  // если мы не в процессе отсыла данных в IoT
#endif
   
    line.startsWith(F("+IPD")))
  {
    ProcessQuery(line); // разбираем пришедшую команду
  } // if

  
  switch(currentAction)
  {
    case wfaWantReady:
    {
      // ждём ответа "ready" от модуля
      if(line == F("ready")) // получили
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => ESP restarted."),currentAction);
          CHECK_QUEUE_TAIL(wfaWantReady);
       #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду
       currentAction = wfaIdle;
      }
    }
    break;

    case wfaCheckModemHang:
    {
      // проверяли, отвечает ли модем
      if(IsKnownAnswer(line)) 
      {
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => ESP answered and available."),currentAction);
          CHECK_QUEUE_TAIL(wfaCheckModemHang);
         #endif
         
         actionsQueue.pop(); // убираем последнюю обработанную команду     
         currentAction = wfaIdle;

         if(flags.wantReconnect)
         {
    
           #ifdef WIFI_DEBUG
            WIFI_DEBUG_WRITE(F("No connection, try to reconnect..."),currentAction);
           #endif

            #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
              EnsureIoTProcessed();
            #endif

            EnsureHTTPProcessed(ERROR_CANT_ESTABLISH_CONNECTION);
                             
            flags.wantReconnect = false;
            InitQueue();
            needToWaitTimer = 5000; // попробуем через 5 секунд подконнеститься
         }

      }

      if(line == F("No AP"))
      {
        // никуда не подсоединены, пытаемся переподключиться, как только придёт OK
        flags.wantReconnect = true;
      }
      
    }
    break;    

    case wfaEchoOff: // выключили эхо
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => ECHO OFF processed."),currentAction);
          CHECK_QUEUE_TAIL(wfaEchoOff);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
    }
    break;

    case wfaCWMODE: // перешли в смешанный режим
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => SoftAP mode is ON."),currentAction);
          CHECK_QUEUE_TAIL(wfaCWMODE);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
      
    }
    break;

    case wfaCWSAP: // создали точку доступа
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => access point created."),currentAction);
          CHECK_QUEUE_TAIL(wfaCWSAP);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
      
    }
    break;

    case wfaCIPMODE: // установили режим работы сервера
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => TCP-server mode now set to 0."),currentAction);
          CHECK_QUEUE_TAIL(wfaCIPMODE);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
      
    }
    break;

    case wfaCIPMUX: // разрешили множественные подключения
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => Multiple connections allowed."),currentAction);
          CHECK_QUEUE_TAIL(wfaCIPMUX);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
      
    }
    break;

    case wfaCIPSERVER: // запустили сервер
    {
       if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => TCP-server started."),currentAction);
          CHECK_QUEUE_TAIL(wfaCIPSERVER);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
     
    }
    break;

    case wfaCWJAP: // законнектились к роутеру
    {
       if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => connected to the router."),currentAction);
          CHECK_QUEUE_TAIL(wfaCWJAP);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
      
    }
    break;

    case wfaCWQAP: // отсоединились от роутера
    {
      if(IsKnownAnswer(line))
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("[OK] => disconnected from router."),currentAction);
          CHECK_QUEUE_TAIL(wfaCWQAP);
        #endif
       actionsQueue.pop(); // убираем последнюю обработанную команду     
       currentAction = wfaIdle;
      }
     
    }
    break;

    //////////////////////////// ЦИКЛ HTTP ////////////////////////////////////////
    case wfaStartHTTPSend: // коннектились к хосту по HTTP
    {
      if(IsKnownAnswer(line))
      {

         #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("HTTP connection command done, parse..."),currentAction);
          CHECK_QUEUE_TAIL(wfaStartHTTPSend);
         #endif

        actionsQueue.pop(); // убираем последнюю обработанную команду
        currentAction = wfaIdle;
        
        // один из известных нам ответов?
        if(line == F("OK"))
        {
         #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("HTTP connection OK, continue..."),currentAction);
         #endif
          // законнектились, можем посылать данные
          actionsQueue.push_back(wfaStartSendHTTPData); // добавляем команду на актуальный отсыл данных в очередь     
        }
        else
        {
         #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("HTTP connection ERROR!"),currentAction);
         #endif

          // всё плохо, вызываем коллбэк
            EnsureHTTPProcessed(ERROR_CANT_ESTABLISH_CONNECTION);
        }
      } // if(IsKnownAnswer(line))      
    }
    break;


    case wfaStartSendHTTPData: // запросили отсыл данных по HTTP, надо прочекать, что да как
    {
      // ждём > для отсыла данных
        #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("HTTP, waiting for \">\"..."),currentAction);
        #endif 

      if(line == F(">")) // дождались приглашения
      {
        #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("\">\" FOUND, sending HTTP-query data..."),currentAction);
          CHECK_QUEUE_TAIL(wfaStartSendHTTPData);
        #endif 

        actionsQueue.pop(); // убираем последнюю обработанную команду
        actionsQueue.push_back(wfaActualSendHTTPData); // добавляем команду на актуальный отсыл данных в очередь     
        currentAction = wfaIdle;
        flags.inSendData = true; // выставляем флаг, что мы отсылаем данные, и тогда очередь обработки клиентов не будет чухаться
               
      }
      else
      if(line.indexOf(F("FAIL")) != -1 || line.indexOf(F("ERROR")) != -1)
      {
         // всё плохо 
        #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("Error sending HTTP-query data!"),currentAction);
          CHECK_QUEUE_TAIL(wfaStartSendHTTPData);
        #endif 
        actionsQueue.pop(); // убираем последнюю обработанную команду
        currentAction = wfaIdle; // переходим в ждущий режим
        // поскольку мы законнекчены - надо закрыть соединение
        actionsQueue.push_back(wfaCloseHTTPConnection);

        // убеждаемся, что мы уведомили вызвавшую сторону о результатах запроса
        EnsureHTTPProcessed(ERROR_HTTP_REQUEST_FAILED);
      }
    }
    break;  

    case wfaCloseHTTPConnection: // закрывали HTTP-соединение
    {
      if(IsKnownAnswer(line)) // дождались закрытия соединения
      {
        #ifdef HTTP_DEBUG
        WIFI_DEBUG_WRITE(F("HTTP connection closed."),currentAction);
        CHECK_QUEUE_TAIL(wfaCloseHTTPConnection);
        #endif
        actionsQueue.pop(); // убираем последнюю обработанную команду     
        currentAction = wfaIdle;
        flags.inSendData = false; // разрешаем обработку других клиентов
      }
    }
    break;

    case wfaActualSendHTTPData:
    {
      // мы тут, понимаешь ли, ждём ответа на отсыл данных  HTTP-запроса.
      // в порт нам сыпется всё подряд, мы пересылаем это вызвавшей стороне до тех пор, пока она скажет "хватит",
      // или пока ESP нам не скажет, что наш клиент отвалился.

      // сначала проверяем, не закрыто ли соединение?
       int idx = line.indexOf(F(",CLOSED"));
       
       if(idx != -1) // соединение закрыто сервером, всё отослали
        {
          // клиент отсоединился
          String s = line.substring(0,idx);
          int clientID = s.toInt();
          if(clientID >= 0 && clientID < MAX_WIFI_CLIENTS)
          {
            // проверяем - не наш ли клиент?
            if(clientID == (MAX_WIFI_CLIENTS - 1))
            {

               #ifdef HTTP_DEBUG
                  WIFI_DEBUG_WRITE(F("HTTP client disconnected, DONE."),currentAction);
               #endif 
                            
                // наш - значит, мы уже весь ответ вычитали, и можем рапортовать о завершении
                EnsureHTTPProcessed(HTTP_REQUEST_COMPLETED);
                
                actionsQueue.pop(); // убираем последнюю обработанную команду
                currentAction = wfaIdle;
                flags.inSendData = false; // разрешаем обработку других клиентов         

            } // if
            
          }
        } // if
       else
       {
          bool enough = false;
          httpHandler->OnAnswerLineReceived(line,enough);
    
          // смотрим, может, хватит?
          if(enough)
          {
             // точно, хватит
             #ifdef HTTP_DEBUG
                WIFI_DEBUG_WRITE(F("HTTP request done."),currentAction);
             #endif
    
             // говорим, что всё на мази
             EnsureHTTPProcessed(HTTP_REQUEST_COMPLETED);
    
             // и закрываем соединение
              actionsQueue.pop(); // убираем последнюю обработанную команду
              currentAction = wfaIdle;         
              
              // поскольку мы законнекчены - надо закрыть соединение
              actionsQueue.push_back(wfaCloseHTTPConnection);              
          } // if(enough)
      } // else not closed
    }
    break;

    //////////////////////////// ЦИКЛ HTTP КОНЧИЛСЯ ////////////////////////////////////////
    
 
 #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)

    case wfaActualSendIoTData:
    {
      // мы тут, понимаешь ли, ждём ответа на отсыл данных в IoT.
      // Ждём до тех пор, пока не получен известный нам ответ или строка не начинается с +IPD
      bool isIpd = line.startsWith(F("+IPD"));
      if(isIpd)
      {
        // дождались, следовательно, можем вызывать коллбэк, сообщая, что мы успешно отработали
          #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT data processed, parse answer"),currentAction);
          CHECK_QUEUE_TAIL(wfaActualSendIoTData);
         #endif

            #ifdef WIFI_DEBUG
              WIFI_DEBUG_WRITE(F("IoT SUCCESS!"),currentAction);
            #endif
            // хорошо
            EnsureIoTProcessed(true);
         
          // в любом случае - завершаем обработку
          actionsQueue.pop(); // убираем последнюю обработанную команду
          currentAction = wfaIdle;         
          
          // поскольку мы законнекчены - надо закрыть соединение
          actionsQueue.push_back(wfaCloseIoTConnection);          
       }
       else
       {
          if(line.endsWith(F("SEND FAIL"))) // не удалось послать данные
          {
              actionsQueue.pop(); // убираем последнюю обработанную команду
              currentAction = wfaIdle;         
              EnsureIoTProcessed();
          }
       } // else
    }
    break;

    case wfaStartIoTSend:
    {
      // ждём коннекта к серверу
      if(IsKnownAnswer(line))
      {

         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT connection command done, parse..."),currentAction);
          CHECK_QUEUE_TAIL(wfaStartIoTSend);
         #endif

        actionsQueue.pop(); // убираем последнюю обработанную команду
        currentAction = wfaIdle;
        
        // один из известных нам ответов
        if(line == F("OK"))
        {
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT connection OK, continue..."),currentAction);
         #endif
          // законнектились, можем посылать данные
          actionsQueue.push_back(wfaStartSendIoTData); // добавляем команду на актуальный отсыл данных в очередь     
        }
        else
        {
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT connection ERROR!"),currentAction);
         #endif

            flags.inSendData = false;
          // всё плохо, вызываем коллбэк
            EnsureIoTProcessed();
        }
      } // if(IsKnownAnswer(line))
    }
    break;

    case wfaCloseIoTConnection:
    {
      if(IsKnownAnswer(line)) // дождались закрытия соединения
      {
        #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("IoT connection closed."),currentAction);
        CHECK_QUEUE_TAIL(wfaCloseIoTConnection);
        #endif
        actionsQueue.pop(); // убираем последнюю обработанную команду     
        currentAction = wfaIdle;
        flags.inSendData = false; // разрешаем обработку других клиентов
      }
    }
    break;

    case wfaStartSendIoTData:
    {
      // ждём > для отсыла данных
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT, waiting for \">\"..."),currentAction);
        #endif 

      if(line == F(">")) // дождались приглашения
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("\">\" FOUND, sending data to IoT..."),currentAction);
          CHECK_QUEUE_TAIL(wfaStartSendIoTData);
        #endif 

        actionsQueue.pop(); // убираем последнюю обработанную команду
        actionsQueue.push_back(wfaActualSendIoTData); // добавляем команду на актуальный отсыл данных в очередь     
        currentAction = wfaIdle;
        flags.inSendData = true; // выставляем флаг, что мы отсылаем данные, и тогда очередь обработки клиентов не будет чухаться
               
      }
      else
      if(line.indexOf(F("FAIL")) != -1 || line.indexOf(F("ERROR")) != -1)
      {
         // всё плохо 
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Error sending data to IoT!"),currentAction);
          CHECK_QUEUE_TAIL(wfaStartSendIoTData);
        #endif 
        actionsQueue.pop(); // убираем последнюю обработанную команду
        currentAction = wfaIdle; // переходим в ждущий режим
        // поскольку мы законнекчены - надо закрыть соединение
        actionsQueue.push_back(wfaCloseIoTConnection);
        EnsureIoTProcessed();
      }
    }
    break;
#endif // IOT

    case wfaCIPSEND: // надо отослать данные клиенту
    {
      // wfaCIPSEND плюёт в очередь функция UpdateClients, перед отсылкой команды модулю.
      // значит, мы сами должны разрулить ситуацию, как быть с обработкой этой команды. 
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Waiting for \">\"..."),currentAction);
        #endif        
            
      if(line == F(">")) // дождались приглашения
      {
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("\">\" FOUND, sending the data..."),currentAction);
          CHECK_QUEUE_TAIL(wfaCIPSEND);
        #endif        
        actionsQueue.pop(); // убираем последнюю обработанную команду (wfaCIPSEND, которую плюнула в очередь функция UpdateClients)
        actionsQueue.push_back(wfaACTUALSEND); // добавляем команду на актуальный отсыл данных в очередь     
        currentAction = wfaIdle;
        flags.inSendData = true; // выставляем флаг, что мы отсылаем данные, и тогда очередь обработки клиентов не будет чухаться
      }
      else
      if(line.indexOf(F("FAIL")) != -1 || line.indexOf(F("ERROR")) != -1)
      {
        // передача данных клиенту неудачна, отсоединяем его принудительно
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Closing client connection unexpectedly!"),currentAction);
          CHECK_QUEUE_TAIL(wfaCIPSEND);
        #endif 
                
        clients[currentClientIDX].SetConnected(false); // выставляем текущему клиенту статус "отсоединён"
        actionsQueue.pop(); // убираем последнюю обработанную команду (wfaCIPSEND, которую плюнула в очередь функция UpdateClients)
        currentAction = wfaIdle; // переходим в ждущий режим
        flags.inSendData = false;
      }
    }
    break;

    case wfaACTUALSEND: // отослали ли данные?
    {
      // может ли произойти ситуация, когда в очереди есть wfaACTUALSEND, помещенная туда обработчиком wfaCIPSEND,
      // но до Update дело ещё не дошло? Считаем, что нет. Мы попали сюда после функции Update, которая в обработчике wfaACTUALSEND
      // отослала нам пакет данных. Надо проверить результат отсылки.
      if(IsKnownAnswer(line)) // получен результат отсылки пакета
      {
        #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("DATA SENT, go to IDLE mode..."),currentAction);
        // проверяем валидность того, что в очереди
        CHECK_QUEUE_TAIL(wfaACTUALSEND);
        #endif
        actionsQueue.pop(); // убираем последнюю обработанную команду (wfaACTUALSEND, которая в очереди)    
        currentAction = wfaIdle; // разрешаем обработку следующего клиента
        flags.inSendData = false; // выставляем флаг, что мы отправили пакет, и можем обрабатывать следующего клиента
        if(!clients[currentClientIDX].HasPacket())
        {
           // данные у клиента закончились
        #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(String(F("No packets in client #")) + String(currentClientIDX),currentAction);
        #endif

         #ifndef WIFI_TCP_KEEP_ALIVE  // если надо разрывать соединение после отсыла результатов - разрываем его
          if(clients[currentClientIDX].IsConnected())
          {
            #ifdef WIFI_DEBUG
            WIFI_DEBUG_WRITE(String(F("Client #")) + String(currentClientIDX) + String(F(" has no packets, closing connection...")),currentAction);
            #endif
            actionsQueue.push_back(wfaCIPCLOSE); // добавляем команду на закрытие соединения
            flags.inSendData = true; // пока не обработаем отсоединение клиента - не разрешаем посылать пакеты другим клиентам
          } // if
        #endif  
        
        } // if
      
        if(line.indexOf(F("FAIL")) != -1 || line.indexOf(F("ERROR")) != -1)
        {
          // передача данных клиенту неудачна, отсоединяем его принудительно
           #ifdef WIFI_DEBUG
            WIFI_DEBUG_WRITE(F("Closing client connection unexpectedly!"),currentAction);
          #endif 
                  
          clients[currentClientIDX].SetConnected(false);
        }      

      } // if known answer

    }
    break;

    case wfaCIPCLOSE: // закрыли соединение
    {
      if(IsKnownAnswer(line)) // дождались приглашения
      {
        #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Client connection closed."),currentAction);
        CHECK_QUEUE_TAIL(wfaCIPCLOSE);
        #endif
        clients[currentClientIDX].SetConnected(false);
        actionsQueue.pop(); // убираем последнюю обработанную команду     
        currentAction = wfaIdle;
        flags.inSendData = false; // разрешаем обработку других клиентов
      }
    }
    break;

    case wfaIdle:
    {
    }
    break;
  } // switch

  // смотрим, может - есть статус клиента
  int idx = line.indexOf(F(",CONNECT"));
  if(idx != -1)
  {
    // клиент подсоединился
    String s = line.substring(0,idx);
    int clientID = s.toInt();
    if(clientID >= 0 && clientID < MAX_WIFI_CLIENTS-1) // последнему клиенту не даём статус законнекченного
    {
   #ifdef WIFI_DEBUG
    WIFI_DEBUG_WRITE(String(F("[CLIENT CONNECTED] - ")) + s,currentAction);
   #endif     
      clients[clientID].SetConnected(true);
    }
  } // if
  idx = line.indexOf(F(",CLOSED"));
 if(idx != -1)
  {
    // клиент отсоединился
    String s = line.substring(0,idx);
    int clientID = s.toInt();
    if(clientID >= 0 && clientID < MAX_WIFI_CLIENTS)
    {
   #ifdef WIFI_DEBUG
   WIFI_DEBUG_WRITE(String(F("[CLIENT DISCONNECTED] - ")) + s,currentAction);
   #endif     
      clients[clientID].SetConnected(false);

      if(clientID == MAX_WIFI_CLIENTS-1)
      {
        #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
          EnsureIoTProcessed();
        #endif
      }
      
    }
  } // if
  
  
}
//--------------------------------------------------------------------------------------------------------------------------------
bool WiFiModule::CanMakeQuery() // тестирует, может ли модуль сейчас сделать запрос
{
  //TODO: УБРАТЬ, ЭТО ТОЛЬКО ДЛЯ ЦЕЛЕЙ ТЕСТИРОВАНИЯ провайдера GSM, поскольку Wi-Fi имеет более высокий приоритет!!!
  //return false;
  
  if(flags.inSendData || 
    flags.inRebootMode || 
    flags.wantIoTToProcess || 
    flags.wantHTTPRequest || 
    flags.inHTTPRequestMode || 
    !flags.isAnyAnswerReceived ||
    actionsQueue.size())
  {
    // не можем обработать запрос

    return false;
  }

  return flags.isConnected;
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::MakeQuery(HTTPRequestHandler* handler) // начинаем запрос по HTTP
{
    // сперва завершаем обработку предыдущего вызова, если он вдруг нечаянно был
    EnsureHTTPProcessed(ERROR_HTTP_REQUEST_CANCELLED);

    // сохраняем обработчик запроса у себя
    httpHandler = handler;

    // и говорим, что мы готовы работать по HTTP-запросу
    flags.wantHTTPRequest = true;
}
//--------------------------------------------------------------------------------------------------------------------------------  
void WiFiModule::ProcessQuery(const String& command)
{
  
  int idx = command.indexOf(F(",")); // ищем первую запятую после +IPD
  const char* ptr = command.c_str();
  ptr += idx+1;
  // перешли за запятую, парсим ID клиента
  String connectedClientID = F("");
  while(*ptr != ',')
  {
    connectedClientID += (char) *ptr;
    ptr++;
  }
  ptr++; // за запятую
  String dataLen;
  while(*ptr != ':')
  {
    dataLen += (char) *ptr;
    ptr++; // перешли на начало данных
  }
  
  ptr++; // за двоеточие

  // тут пришла команда, разбираем её
  ProcessCommand(connectedClientID.toInt(),dataLen.toInt(),ptr);
   
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::ProcessCommand(int clientID, int dataLen, const char* command)
{
  // обрабатываем команду, пришедшую по TCP/IP
  
 #ifdef WIFI_DEBUG
  WIFI_DEBUG_WRITE(String(F("Client ID = ")) + String(clientID) + String(F("; len= ")) + String(dataLen),currentAction);
  WIFI_DEBUG_WRITE(String(F("Requested command: ")) + String(command),currentAction);
#endif
  
  // работаем с клиентом
  if(clientID >=0 && clientID < MAX_WIFI_CLIENTS-1) // последний клиент - наш, для IoT и HTTP - в него не пишем
  {


      if(!*command) // пустой пакет, с переводом строки
        dataLen = 0;

        // теперь нам надо сложить все данные в клиента - как только он получит полный пакет - он подготовит
        // все данные к отправке. Признаком конца команды к контроллеру у нас служит перевод строки \r\n.
        // следовательно, пока мы не получим в любом виде перевод строки - считается, что команда не получена.
        // перевод строки может быть либо получен прямо в данных, либо - в следующем пакете.

        // как только клиент накопит всю команду - он получает данные с контроллера в следующем вызове Update.
        clients[clientID].CommandRequested(dataLen,command); // говорим клиенту, чтобы сложил во внутренний буфер
  } // if
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::Setup()
{
  // настройка модуля тут

 // сообщаем, что мы провайдер HTTP-запросов
 #ifdef USE_WIFI_MODULE_AS_HTTP_PROVIDER
  MainController->SetHTTPProvider(0,this); 
 #endif
  
  for(uint8_t i=0;i<MAX_WIFI_CLIENTS;i++)
    clients[i].Setup(i, WIFI_PACKET_LENGTH);



  #ifdef USE_WIFI_REBOOT_PIN
    WORK_STATUS.PinMode(WIFI_REBOOT_PIN,OUTPUT);
    WORK_STATUS.PinWrite(WIFI_REBOOT_PIN,WIFI_POWER_ON);
  #endif

  // поднимаем сериал
  WIFI_SERIAL.begin(WIFI_BAUDRATE);

  if(&(WIFI_SERIAL) == &Serial) {
       WORK_STATUS.PinMode(0,INPUT_PULLUP,true);
       WORK_STATUS.PinMode(1,OUTPUT,false);
  } else if(&(WIFI_SERIAL) == &Serial1) {
       WORK_STATUS.PinMode(19,INPUT_PULLUP,true);
       WORK_STATUS.PinMode(18,OUTPUT,false);
  } else if(&(WIFI_SERIAL) == &Serial2) {
       WORK_STATUS.PinMode(17,INPUT_PULLUP,true);
       WORK_STATUS.PinMode(16,OUTPUT,false);
  } else if(&(WIFI_SERIAL) == &Serial3) {
       WORK_STATUS.PinMode(15,INPUT_PULLUP,true);
       WORK_STATUS.PinMode(14,OUTPUT,false);
  } 

  flags.isAnyAnswerReceived = false;
  flags.inRebootMode = false;
  flags.wantIoTToProcess = false;
  flags.wantHTTPRequest = false;
  flags.inHTTPRequestMode = false;
  httpHandler = NULL;
  httpData = NULL;
  
  rebootStartTime = 0;

  InitQueue(); // инициализируем очередь


#if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)

     iotWriter = NULL;
     iotDone = NULL;
     iotDataHeader = NULL;
     iotDataFooter = NULL;
     iotDataLength = 0;     
     IoTList.RegisterGate(this); // регистрируем себя как отсылателя данных в IoT
#endif  


}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::InitQueue(bool addRebootCommand)
{

  while(actionsQueue.size() > 0) // чистим очередь 
    actionsQueue.pop();

  WaitForDataWelcome = false; // не ждём приглашения
  
  nextClientIDX = 0;
  currentClientIDX = 0;
  flags.inSendData = false;
  flags.isConnected = false;

  // инициализируем время отсылки команды и получения ответа
  sendCommandTime = millis();
  answerWaitTimer = 0;

  needToWaitTimer = 0; // сбрасываем таймер

  // настраиваем то, что мы должны сделать
  currentAction = wfaIdle; // свободны, ничего не делаем

GlobalSettings* Settings = MainController->GetSettings();
  
  if(Settings->GetWiFiState() & 0x01) // коннектимся к роутеру
    actionsQueue.push_back(wfaCWJAP); // коннектимся к роутеру совсем в конце
  else  
    actionsQueue.push_back(wfaCWQAP); // отсоединяемся от роутера
    
  actionsQueue.push_back(wfaCIPSERVER); // сервер поднимаем в последнюю очередь
  actionsQueue.push_back(wfaCIPMUX); // разрешаем множественные подключения
  actionsQueue.push_back(wfaCIPMODE); // устанавливаем режим работы
  actionsQueue.push_back(wfaCWSAP); // создаём точку доступа
  actionsQueue.push_back(wfaCWMODE); // // переводим в смешанный режим
  actionsQueue.push_back(wfaEchoOff); // выключаем эхо
  
  if(addRebootCommand)
    actionsQueue.push_back(wfaWantReady); // надо получить ready от модуля путём его перезагрузки    
  
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::SendCommand(const String& command, bool addNewLine)
{
  #ifdef WIFI_DEBUG
    WIFI_DEBUG_WRITE(String(F("==> Send the \"")) + command + String(F("\" command to ESP...")),currentAction);
  #endif

  // запоминаем время отсылки последней команды
  sendCommandTime = millis();
  answerWaitTimer = 0;

  WIFI_SERIAL.write(command.c_str(),command.length());
  
  if(addNewLine)
  {
    WIFI_SERIAL.write(String(NEWLINE).c_str());
  }
      
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::ProcessQueue()
{
  if(currentAction != wfaIdle) // чем-то заняты, не можем ничего делать
    return;

    size_t sz = actionsQueue.size();
    if(!sz) 
    {
      // в очереди ничего нет, можем проверять, что мы можем сделать
      if(flags.wantHTTPRequest)
      {
        // от нас ждут запроса по HTTP
        flags.wantHTTPRequest = false;
        flags.inHTTPRequestMode = true;
        actionsQueue.push_back(wfaStartHTTPSend);

        return; // возвращаемся, здесь делать нефик
      }

      #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)

      if(!flags.inHTTPRequestMode) // только если мы не в режиме отсыла HTTP-запроса
      {
        if(flags.wantIoTToProcess && iotWriter && iotDone)
        {
          // надо поместить в очередь команду на обработку запроса к IoT
          flags.wantIoTToProcess = false;
          flags.inSendData = true; // чтобы не дёргать очередь клиентов
          actionsQueue.push_back(wfaStartIoTSend);
          return;
        }
      }
      #endif
  
      // тут проверяем - можем ли мы протестировать доступность модема?
      if(millis() - sendCommandTime > WIFI_AVAILABLE_CHECK_TIME) 
      {
          // раз в минуту можно проверить доступность модема,
          // и делаем мы это ТОЛЬКО тогда, когда очередь пуста как минимум WIFI_AVAILABLE_CHECK_TIME мс, т.е. все текущие команды отработаны.
          actionsQueue.push_back(wfaCheckModemHang);
      }      
      return;
  } // if(!sz)
      
    currentAction = actionsQueue[sz-1]; // получаем очередную команду

    // смотрим, что за команда
    switch(currentAction)
    {

      //////////////////////////// ЦИКЛ HTTP ////////////////////////////////////////
      
      case wfaStartHTTPSend: // начинаем запрос по HTTP
      {
        #ifdef HTTP_DEBUG
          Serial.println(F("Start HTTP connection..."));
        #endif

        // получаем адрес хоста
        String host;
        httpHandler->OnAskForHost(host);

        // теперь формируем команду
        String comm = F("AT+CIPSTART=");
        comm += String(MAX_WIFI_CLIENTS - 1); // коннектимся последним клиентом
        comm += F(",\"TCP\",\"");
        comm += host;
        comm += F("\",80");

        // и отсылаем её
        SendCommand(comm);
      }
      break;

      case wfaStartSendHTTPData: // можем отсылать данные по HTTP
      {
        #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("Make HTTP query AND send data..."),currentAction);
        #endif        
        // тут посылаем команду на отсыл данных по HTTP
        // но сначала - запросим-ка мы данные у вызвавшей всю эту движуху стороны
        delete httpData;
        httpData = new String;

        httpHandler->OnAskForData(httpData); // получили данные, которые надо отослать
        
        String comm = CIPSEND_COMMAND;
        comm += String(MAX_WIFI_CLIENTS-1); // коннектимся последним клиентом
        comm += F(",");
        comm += httpData->length();
        WaitForDataWelcome = true; // выставляем флаг, что мы ждём >
        SendCommand(comm);
                
      }
      break;

      case wfaCloseHTTPConnection: // закрываем HTTP-соединение
      {
        // надо закрыть соединение
        #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("Closing HTTP connection..."),currentAction);
        #endif

        flags.inSendData = true;
        String comm = F("AT+CIPCLOSE=");
        comm += String(MAX_WIFI_CLIENTS-1);
        SendCommand(comm);
                          
      }
      break;

      case wfaActualSendHTTPData: // начинаем отсылать данные HTTP-запроса
      {
         #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("Send HTTP-query data..."),currentAction);
        #endif  
          if(httpData)
          {      
            // тут посылаем данные
            SendCommand(*httpData,false);
            delete httpData;
            httpData = NULL;
          }
          else
          {
         #ifdef HTTP_DEBUG
          WIFI_DEBUG_WRITE(F("HTTP-query data is INVALID!"),currentAction);
        #endif  
        // чего-то в процессе не задалось, вызываем коллбэк, и говорим, что всё плохо
            EnsureHTTPProcessed(ERROR_HTTP_REQUEST_FAILED);
            actionsQueue.pop();
            currentAction = wfaIdle;
            flags.inSendData = false; 
          }
      }
      break;
      
      //////////////////////////// ЦИКЛ HTTP КОНЧИЛСЯ ////////////////////////////////////////      

      
      #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)

      case wfaActualSendIoTData:
      {
        // отсылаем данные в IoT
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Send data to IoT using ESP..."),currentAction);
        #endif  
          if(iotDataHeader && iotDataFooter && iotWriter && iotDone)
          {      
            // тут посылаем данные в IoT
            SendCommand(*iotDataHeader,false);
            iotWriter(&(WIFI_SERIAL));
            SendCommand(*iotDataFooter,false);       
          }
          else
          {
         #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IoT data is INVALID!"),currentAction);
        #endif  
        // чего-то в процессе не задалось, вызываем коллбэк, и говорим, что всё плохо
            EnsureIoTProcessed();
            actionsQueue.pop();
            currentAction = wfaIdle;
            flags.inSendData = false; 
          }
      }
      break;

      case wfaStartSendIoTData:
      {

        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Start sending data to IoT using ESP..."),currentAction);
        #endif        
        // тут посылаем команду на отсыл данных в IoT
        String comm = CIPSEND_COMMAND;
        comm += String(MAX_WIFI_CLIENTS-1); // коннектимся последним клиентом
        comm += F(",");
        comm += iotDataLength;
        WaitForDataWelcome = true; // выставляем флаг, что мы ждём >
        SendCommand(comm);
      }
      break;

      case wfaStartIoTSend:
      {
          // надо отослать данные в IOT
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Connect to IoT using ESP..."),currentAction);
        #endif
    
        String comm = F("AT+CIPSTART=");
        comm += String(MAX_WIFI_CLIENTS - 1); // коннектимся последним клиентом
        comm += F(",\"TCP\",\"");

          // смотрим, в какой сервис запросили отсыл данных
          switch(iotService)
          {
            case iotThingSpeak:
              comm += THINGSPEAK_IP;
            break;
          }
        comm += F("\",80");
        SendCommand(comm);   
      }     
      break;

      case wfaCloseIoTConnection:
      {
        // надо закрыть соединение
        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("Closing IoT connection..."),currentAction);
        #endif

        flags.inSendData = true;
        String comm = F("AT+CIPCLOSE=");
        comm += String(MAX_WIFI_CLIENTS-1);
        SendCommand(comm);
                          
      }
      break;

     #endif
      
      case wfaWantReady:
      {
        // надо рестартовать модуль
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Restart the ESP..."),currentAction);
      #endif
      SendCommand(F("AT+RST"));
      }
      break;

      case wfaCheckModemHang:
      {
          // проверяем, не завис ли модем?
        #ifdef WIFI_DEBUG
          Serial.println(F("Check if modem available..."));
        #endif

        flags.wantReconnect = false;
        SendCommand(F("AT+CWJAP?")); // проверяем, подконнекчены ли к роутеру
      }
      break;

      case wfaEchoOff:
      {
        // выключаем эхо
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Disable echo..."),currentAction);
      #endif
      SendCommand(F("ATE0"));
      //SendCommand(F("AT+CIOBAUD=230400")); // переводим на другую скорость
      }
      break;

      case wfaCWMODE:
      {
        // переходим в смешанный режим
      #ifdef WIFI_DEBUG
       WIFI_DEBUG_WRITE(F("Go to SoftAP mode..."),currentAction);
      #endif
      SendCommand(F("AT+CWMODE_DEF=3"));
      }
      break;

      case wfaCWSAP: // создаём точку доступа
      {

      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Creating the access point..."),currentAction);
      #endif

        GlobalSettings* Settings = MainController->GetSettings();
      
        String com = F("AT+CWSAP_DEF=\"");
        com += Settings->GetStationID();
        com += F("\",\"");
        com += Settings->GetStationPassword();
        com += F("\",8,4");
        
        SendCommand(com);
        
      }
      break;

      case wfaCIPMODE: // устанавливаем режим работы сервера
      {
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Set the TCP server mode to 0..."),currentAction);
      #endif
      SendCommand(F("AT+CIPMODE=0"));
      
      }
      break;

      case wfaCIPMUX: // разрешаем множественные подключения
      {
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Allow the multiple connections..."),currentAction);
      #endif
      SendCommand(F("AT+CIPMUX=1"));
        
      }
      break;

      case wfaCIPSERVER: // запускаем сервер
      {  
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Starting TCP-server..."),currentAction);
      #endif
      SendCommand(F("AT+CIPSERVER=1,1975"));
      
      }
      break;

      case wfaCWQAP: // отсоединяемся от точки доступа
      {  
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Disconnect from router..."),currentAction);
      #endif
      SendCommand(F("AT+CWQAP"));
      
      }
      break;

      case wfaCWJAP: // коннектимся к роутеру
      {
      #ifdef WIFI_DEBUG
        WIFI_DEBUG_WRITE(F("Connecting to the router..."),currentAction);
      #endif

        GlobalSettings* Settings = MainController->GetSettings();
        
        String com = F("AT+CWJAP_DEF=\"");
        com += Settings->GetRouterID();
        com += F("\",\"");
        com += Settings->GetRouterPassword();
        com += F("\"");
        SendCommand(com);

      }
      break;

      case wfaCIPSEND: // надо отослать данные клиенту
      {
        #ifdef WIFI_DEBUG
       //  WIFI_DEBUG_WRITE(F("ASSERT: wfaCIPSEND in ProcessQueue!"),currentAction);
        #endif
        
      }
      break;

      case wfaACTUALSEND: // дождались приглашения в функции ProcessAnswerLine, она поместила команду wfaACTUALSEND в очередь - отсылаем данные клиенту
      {
            #ifdef WIFI_DEBUG
              WIFI_DEBUG_WRITE(String(F("Sending data to the client #")) + String(currentClientIDX),currentAction);
            #endif
      
            if(clients[currentClientIDX].IsConnected()) // не отвалился ли клиент?
            {
              // клиент по-прежнему законнекчен, посылаем данные
              if(!clients[currentClientIDX].SendPacket(&(WIFI_SERIAL)))
              {
                // если мы здесь - то пакетов у клиента больше не осталось. Надо дождаться подтверждения отсылки последнего пакета
                // в функции ProcessAnswerLine (обработчик wfaACTUALSEND), и послать команду на закрытие соединения с клиентом.
              #ifdef WIFI_DEBUG
              WIFI_DEBUG_WRITE(String(F("All data to the client #")) + String(currentClientIDX) + String(F(" has sent, need to wait for last packet sent..")),currentAction);
              #endif
 
              }
              else
              {
                // ещё есть пакеты, продолжаем отправлять в следующих вызовах Update
              #ifdef WIFI_DEBUG
              WIFI_DEBUG_WRITE(String(F("Client #")) + String(currentClientIDX) + String(F(" has ")) + String(clients[currentClientIDX].GetPacketsLeft()) + String(F(" packets left...")),currentAction);
              #endif
              } // else
            } // is connected
            else
            {
              // клиент отвалится, чистим...
            #ifdef WIFI_DEBUG
              WIFI_DEBUG_WRITE(F("Client disconnected, clear the client data..."),currentAction);
            #endif
              clients[currentClientIDX].SetConnected(false);
            }

      }
      break;

      case wfaCIPCLOSE: // закрываем соединение с клиентом
      {
        if(clients[currentClientIDX].IsConnected()) // только если клиент законнекчен 
        {
          #ifdef WIFI_DEBUG
            WIFI_DEBUG_WRITE(String(F("Closing client #")) + String(currentClientIDX) + String(F(" connection...")),currentAction);
          #endif
          clients[currentClientIDX].SetConnected(false);
          String command = F("AT+CIPCLOSE=");
          command += currentClientIDX; // закрываем соединение
          SendCommand(command);
        }
        
        else
        {
          #ifdef WIFI_DEBUG
            WIFI_DEBUG_WRITE(String(F("Client #")) + String(currentClientIDX) + String(F(" already broken!")),currentAction);
            CHECK_QUEUE_TAIL(wfaCIPCLOSE);
          #endif
          // просто убираем команду из очереди
           actionsQueue.pop();
           currentAction = wfaIdle; // разрешаем обработку следующей команды
           flags.inSendData = false; // разрешаем обработку следующего клиента
        } // else
        
      }
      break;

      case wfaIdle:
      {
        // ничего не делаем

      }
      break;
      
    } // switch
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::RebootModem()
{
  // перезагружаем модем тут
  #ifdef WIFI_DEBUG
    Serial.println(F("[ERR] - ESP not answering, reboot it..."));
  #endif

  // мы в процессе перезагрузки
  flags.inRebootMode = true;

  // запоминаем время выключения питания
  rebootStartTime = millis();

  //Тут выключение питания модема
  #ifdef USE_WIFI_REBOOT_PIN
    WORK_STATUS.PinWrite(WIFI_REBOOT_PIN,WIFI_POWER_OFF);
  #endif

    
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::UpdateClients()
{
  if(currentAction != wfaIdle || flags.inSendData || flags.inHTTPRequestMode) // чем-то заняты, не можем ничего делать
    return;
    
  // тут ищем, какой клиент сейчас хочет отослать данные

  for(uint8_t idx = nextClientIDX;idx < MAX_WIFI_CLIENTS; idx++)
  { 
    ++nextClientIDX; // переходим на следующего клиента, как только текущему будет послан один пакет

    clients[idx].Update(); // обновляем внутреннее состояние клиента - здесь он может подготовить данные к отправке, например
    
    if(clients[idx].IsConnected() && clients[idx].HasPacket())
    {
      currentAction = wfaCIPSEND; // говорим однозначно, что нам надо дождаться >
      actionsQueue.push_back(wfaCIPSEND); // добавляем команду отсылки данных в очередь
      
    #ifdef WIFI_DEBUG
      WIFI_DEBUG_WRITE(F("Sending data command to the ESP..."),currentAction);
    #endif
  
      // клиент подсоединён и ждёт данных от нас - отсылаем ему следующий пакет
      currentClientIDX = idx; // сохраняем номер клиента, которому будем посылать данные
      String command = CIPSEND_COMMAND;
      command += String(idx);
      command += F(",");
      command += String(clients[idx].GetPacketLength());
      WaitForDataWelcome = true; // выставляем флаг, что мы ждём >

      SendCommand(command);
  
      break; // выходим из цикла
    } // if
    
  } // for
  
  if(nextClientIDX >= MAX_WIFI_CLIENTS) // начинаем обработку клиентов сначала
    nextClientIDX = 0;  
}
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::EnsureHTTPProcessed(uint16_t statusCode)
{
  if(!httpHandler) // не было флага запроса HTTP-адреса
    return;

    #ifdef HTTP_DEBUG
      Serial.print(F("EnsureHTTPProcessed: "));
      Serial.println(statusCode);
    #endif
  
    
   httpHandler->OnHTTPResult(statusCode); // сообщаем, что мы закончили обработку

  flags.wantHTTPRequest = false;
  flags.inHTTPRequestMode = false;
  flags.inSendData = false;
  httpHandler = NULL;
  delete httpData;
  httpData = NULL;
}
//--------------------------------------------------------------------------------------------------------------------------------
#if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
void WiFiModule::EnsureIoTProcessed(bool success)
{
     if(iotDone) 
     {
        // да, нас вызывали для отсыла данных в IoT, но что-то пошло не так
        iotDone({success,iotService});
        iotDone = NULL;
        iotWriter = NULL;
     }

     delete iotDataHeader;
     iotDataHeader = NULL;

     delete iotDataFooter;
     iotDataFooter = NULL;
     
     iotDataLength = 0;  
     flags.wantIoTToProcess = false;         
  
}
#endif
//--------------------------------------------------------------------------------------------------------------------------------
void WiFiModule::Update(uint16_t dt)
{ 
  if(flags.inRebootMode) {
    // мы в процессе перезагрузки модема, надо проверить, пора ли включать питание?
    if(millis() - rebootStartTime > WIFI_REBOOT_TIME) {
      // две секунды держали питание выключенным, можно включать
      flags.inRebootMode = false;
      flags.isAnyAnswerReceived = false; // говорим, что мы ничего от модема не получали

      InitQueue(); // инициализируем очередь

      // ТУТ включение питания модема 
      #ifdef USE_WIFI_REBOOT_PIN
        WORK_STATUS.PinWrite(WIFI_REBOOT_PIN,WIFI_POWER_ON);
      #endif
      needToWaitTimer = WIFI_WAIT_AFTER_REBOOT_TIME; // дадим модему WIFI_WAIT_AFTER_REBOOT_TIME мс на раздупление, прежде чем начнём что-либо делать

      #ifdef WIFI_DEBUG
        Serial.println(F("[REBOOT] - ESP rebooted, wait for ready..."));
      #endif
    }
    
    return;
  }  

  if(needToWaitTimer > 0) // надо ждать следующей команды запрошенное время
  {
    needToWaitTimer -= dt;
    return;
  }

  needToWaitTimer = 0; // сбрасываем таймер ожидания  

   if(currentAction != wfaIdle) // только если мы в процессе обработки команды, то
    answerWaitTimer += dt; // увеличиваем время ожидания ответа на последнюю команду 


   // сначала проверяем - а не слишком ли долго мы ждём ответа от модема?
  if(answerWaitTimer > WIFI_MAX_ANSWER_TIME) {

     // тут смотрим - возможно, нам надо вызвать функцию обратного вызова для IoT
     #if defined(USE_IOT_MODULE) && defined(USE_WIFI_MODULE_AS_IOT_GATE)
      EnsureIoTProcessed();
     #endif

     // тут убеждаемся, что мы сообщили вызывающей стороне о неуспешном запросе по HTTP
     EnsureHTTPProcessed(ERROR_MODEM_NOT_ANSWERING);
     
      // очень долго, надо перезапустить последнюю команду.
     // причём лучше всего перезапустить всё сначала
     InitQueue();
     needToWaitTimer = WIFI_WAIT_AFTER_REBOOT_TIME; // ещё через 5 секунд попробуем
     sendCommandTime = millis(); // сбросили таймера
     answerWaitTimer = 0;

     if(flags.isAnyAnswerReceived) {
        // получали хоть один ответ от модема - возможно, он завис?
        RebootModem();
        
     } else {
        // ничего не получали, модема не подсоединено?
        #ifdef WIFI_DEBUG
          Serial.println(F("[ERR] - ESP not found, check for presence after short time..."));
        #endif
     }


  }     

  if(!flags.inRebootMode) { // если мы не в процессе перезагрузки - то можем отрабатывать очередь  
    UpdateClients();
    ProcessQueue();
  }

}
//--------------------------------------------------------------------------------------------------------------------------------
bool  WiFiModule::ExecCommand(const Command& command, bool wantAnswer)
{
  UNUSED(wantAnswer);
  
  PublishSingleton = NOT_SUPPORTED;

  if(command.GetType() == ctSET) // установка свойств
  {
    uint8_t argsCnt = command.GetArgsCount();
    if(argsCnt > 0)
    {
      String t = command.GetArg(0);
      if(t == WIFI_SETTINGS_COMMAND) // установить настройки вай-фай
      {
        if(argsCnt > 5)
        {
          GlobalSettings* Settings = MainController->GetSettings();
          
          int shouldConnectToRouter = atoi(command.GetArg(1));
          String routerID = command.GetArg(2);
          String routerPassword = command.GetArg(3);
          String stationID = command.GetArg(4);
          String stationPassword = command.GetArg(5);

          bool shouldReastartAP = Settings->GetStationID() != stationID ||
          Settings->GetStationPassword() != stationPassword;


          Settings->SetWiFiState(shouldConnectToRouter);
          Settings->SetRouterID(routerID);
          Settings->SetRouterPassword(routerPassword);
          Settings->SetStationID(stationID);
          Settings->SetStationPassword(stationPassword);
          
          if(!routerID.length())
            Settings->SetWiFiState(0); // не коннектимся к роутеру

//          Settings->Save(); // сохраняем настройки

          if(Settings->GetWiFiState() & 0x01) // коннектимся к роутеру
            actionsQueue.push_back(wfaCWJAP); // коннектимся к роутеру совсем в конце
          else
            actionsQueue.push_back(wfaCWQAP); // отсоединяемся от роутера

          if(shouldReastartAP) // надо пересоздать точку доступа
          {
            actionsQueue.push_back(wfaCIPSERVER); // сервер поднимаем в последнюю очередь
            actionsQueue.push_back(wfaCIPMUX); // разрешаем множественные подключения
            actionsQueue.push_back(wfaCIPMODE); // устанавливаем режим работы
            actionsQueue.push_back(wfaCWSAP); // создаём точку доступа
          }
           
          
          PublishSingleton.Status = true;
          PublishSingleton = t; 
          PublishSingleton << PARAM_DELIMITER << REG_SUCC;
        }
        else
          PublishSingleton = PARAMS_MISSED; // мало параметров
        
      } // WIFI_SETTINGS_COMMAND
    }
    else
      PublishSingleton = PARAMS_MISSED; // мало параметров
  } // SET
  else
  if(command.GetType() == ctGET) // чтение свойств
  {
    uint8_t argsCnt = command.GetArgsCount();
    if(argsCnt > 0)
    {
      String t = command.GetArg(0);
      
      if(t == IP_COMMAND) // получить данные об IP
      {
        if(currentAction != wfaIdle) // не можем ответить на запрос немедленно
          PublishSingleton = BUSY;
        else
        {
        #ifdef WIFI_DEBUG
         WIFI_DEBUG_WRITE(F("Request for IP info..."),currentAction);
        #endif
        
        
        SendCommand(F("AT+CIFSR"));
        // поскольку у нас serialEvent не основан на прерываниях, на самом-то деле (!),
        // то мы должны получить ответ вот прямо вот здесь, и разобрать его.


        String line; // тут принимаем данные до конца строки
        String apCurrentIP;
        String stationCurrentIP;
        bool  apIpDone = false;
        bool staIpDone = false;
        

        char ch;
        while(1)
        { 
          if(apIpDone && staIpDone) // получили оба IP
            break;
            
          while(WIFI_SERIAL.available())
          {
            ch = WIFI_SERIAL.read();
        
            if(ch == '\r')
              continue;
            
            if(ch == '\n')
            {
              // получили строку, разбираем её
                if(line.startsWith(F("+CIFSR:APIP"))) // IP нашей точки доступа
                 {
                    #ifdef WIFI_DEBUG
                      WIFI_DEBUG_WRITE(F("AP IP found, parse..."),currentAction);
                    #endif
            
                   int idx = line.indexOf("\"");
                   if(idx != -1)
                   {
                      apCurrentIP = line.substring(idx+1);
                      idx = apCurrentIP.indexOf("\"");
                      if(idx != -1)
                        apCurrentIP = apCurrentIP.substring(0,idx);
                      
                   }
                   else
                    apCurrentIP = F("0.0.0.0");

                    apIpDone = true;
                 } // if(line.startsWith(F("+CIFSR:APIP")))
                 else
                  if(line.startsWith(F("+CIFSR:STAIP"))) // IP нашей точки доступа, назначенный роутером
                 {
                    #ifdef WIFI_DEBUG
                      WIFI_DEBUG_WRITE(F("STA IP found, parse..."),currentAction);
                    #endif
            
                   int idx = line.indexOf("\"");
                   if(idx != -1)
                   {
                      stationCurrentIP = line.substring(idx+1);
                      idx = stationCurrentIP.indexOf("\"");
                      if(idx != -1)
                        stationCurrentIP = stationCurrentIP.substring(0,idx);
                      
                   }
                   else
                    stationCurrentIP = F("0.0.0.0");

                    staIpDone = true;
                 } // if(line.startsWith(F("+CIFSR:STAIP")))
             
              line = F("");
            } // ch == '\n'
            else
            {
                  line += ch;
            }
        
         if(apIpDone && staIpDone) // получили оба IP
            break;
 
          } // while
          
        } // while(1)
        


        #ifdef WIFI_DEBUG
          WIFI_DEBUG_WRITE(F("IP info requested."),currentAction);
        #endif

        PublishSingleton.Status = true;
        PublishSingleton = t; 
        PublishSingleton << PARAM_DELIMITER << apCurrentIP << PARAM_DELIMITER << stationCurrentIP;
        } // else not busy
      } // IP_COMMAND
    }
    else
      PublishSingleton = PARAMS_MISSED; // мало параметров
  } // GET

  MainController->Publish(this,command);

  return PublishSingleton.Status;
}
//--------------------------------------------------------------------------------------------------------------------------------

