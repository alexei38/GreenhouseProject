#ifndef _SMS_MODULE_H
#define _SMS_MODULE_H

#include "AbstractModule.h"
#include "Settings.h"
#include "TinyVector.h"

typedef enum
{
  smaIdle, // ничего не делаем, просто ждём
  smaCheckReady, // проверяем готовность (AT+CPAS)
  smaEchoOff, // выключаем эхо (ATE0)
  smaAON, // включаем АОН (AT+CLIP=1)
  smaPDUEncoding, // включаем кодировку PDU (AT+CMGF=0)
  smaSMSSettings, // включаем вывод входящих смс сразу в порт (AT+CNMI=2,2)
  smaWaitReg, // ждём регистрации (AT+CREG?)
  smaHangUp, // кладём трубку (ATH)
  smaStartSendSMS, // начинаем отсылать SMS (AT+CMGS=)
  smaSmsActualSend, // актуальный отсыл SMS
  smaClearAllSMS // очистка всех SMS (AT+CMGD=0,4)
  
} SMSActions;

typedef Vector<SMSActions> SMSActionsVector;

class SMSModule : public AbstractModule // модуль поддержки управления по SMS
{
  private:
    GlobalSettings* Settings;

    SMSActions currentAction; // текущая операция, завершения которой мы ждём
    SMSActionsVector actionsQueue; // что надо сделать, шаг за шагом 
    bool IsKnownAnswer(const String& line); // если ответ нам известный, то возвращает true
    void SendCommand(const String& command, bool addNewLine=true); // посылает команды модулю GSM
    void ProcessQueue(); // разбираем очередь команд
    void InitQueue(); // инициализируем очередь

    String smsToSend; // какое SMS отправить
    String commandToSend; // какую команду сперва отправить для отсыла SMS
    bool waitForSMSInNextLine;

    String queuedWindowCommand; // команда на выполнение управления окнами, должна выполняться только когда окна не в движении
    uint16_t queuedTimer; // таймер, чтобы не дёргать часто проверку состояния окон - это незачем
    void ProcessQueuedWindowCommand(uint16_t dt); // обрабатываем команду управления окнами, помещенную в очередь

    long needToWaitTimer; // таймер ожидания до запроса следующей команды
    bool isModuleRegistered; // зарегистрирован ли модуль у оператора?

    void ProcessIncomingCall(const String& line); // обрабатываем входящий звонок
    void ProcessIncomingSMS(const String& line); // обрабатываем входящее СМС
        
  public:
    SMSModule() : AbstractModule(F("SMS")) {}

    bool ExecCommand(const Command& command);
    void Setup();
    void Update(uint16_t dt);

    void SendStatToCaller(const String& phoneNum);
    void SendSMS(const String& sms);

    void ProcessAnswerLine(const String& line);
    volatile bool WaitForSMSWelcome; // флаг, что мы ждём приглашения на отсыл SMS - > (плохое ООП, негодное :) )

        

};


#endif