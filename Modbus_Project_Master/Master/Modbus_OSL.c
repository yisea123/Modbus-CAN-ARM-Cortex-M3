// Author: Francisco Javier Guzman Jimenez, <dejavits@gmail.com>
#ifndef CAN_Mode
//******************************************************************************
//! \defgroup OSL Modbus OSL
//! \brief Módulo Modbus Over Serial Line.
//!
//! Este Módulo se encarga de implementar las comunicaciones Serie de Modbus, 
//! como un sistema de comunicación entre un Master y uno o varios Slaves.
//! Existen 2 opciones de comunicación; el Master envía a peticiones a un Slave
//! y éste realiza las acciones demandadas y responde o se envía una petición
//! a _todos_ los Slaves (BroadCast) sin que éstos emitan respuesta alguna. En
//! ningún caso se comunican los Slaves entre ellos.
//!
//! Por una parte, éste módulo recoge los mensajes entrantes de los módulos 
//! correspondientes a los modos de comunicación Serie (de momento sólo el RTU 
//! está implementado aunque el programa se encuentra perfectamente adaptado
//! para la adición de un módulo OSL_ASCII); comprueba su corrección y los 
//! envía al nivel de Aplicación Modbus App para el proceso de las acciones
//! correspondientes.
//!
//! Por otro lado gestiona también la Salida de mensajes del Sistema añadiendo
//! a la trama proveniente de App (PDU) el Numero de Slave y el checksum 
//! (CRC/LRC) y traduciendo el mensaje a ASCII, si ese es el modo activo, para
//! formar la trama de mensaje completa o Application Data Unit (ADU).
//******************************************************************************
//! @{

#include "inc/lm3s8962.h"
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/uart.h"
#include "driverlib/timer.h"
#include "Modbus_App.h"
#include "Modbus_OSL.h"                   
#include "Modbus_OSL_RTU.h"

//*****************************************************************************
//
// Variables locales del módulo RTU usadas en el desarrollo del programa para
// la comprobación del comportamiento del sistema y la depuración de errores.
//
//*****************************************************************************

//uint16_t Debug_OSL_OutChar=0,Debug_OSL_IncChar=0, Debug_OSL_Rsp_Resend=0;
//unsigned char Debug_OSL_OutMsg=0, Debug_OSL_IncMsg=0, Debug_OSL_CRC_OK=0;

//*****************************************************************************
//
// Variables globales del módulo OSL.
//
//*****************************************************************************

// Para configurar las comunicaciones.

//! \brief Baudrate de las comunicaciones Serie. Su valor debe corresponder con 
//! uno de los contenidos en _enum_ _Baud_. Por defecto es 19200 bps.
static uint32_t Modbus_OSL_Baudrate;
//! \brief Nº de cuentas para establecer un timer que desborde en un tiempo   
//! suficiente como para que se procese la petición y se reciba la respuesta.
static uint32_t Modbus_OSL_Timeout_R;
//! \brief Nº de cuentas para establecer un timer que desborde en un tiempo   
//! suficiente como para que se procese la petición. Para Mensajes BroadCast.
static uint32_t Modbus_OSL_Timeout_B;
//! Modo de las comunicaciones Serie, RTU o ASCII. Por defecto RTU. 
static enum Modbus_OSL_Modes Modbus_OSL_Mode;

// Para datos de Mensaje y Flags del Sistema.
  
//! Marca los mensajes entrantes como MODBUS_OSL_Frame_OK/MODBUS_OSL_Frame_NOK
static volatile enum Modbus_OSL_Frames Modbus_OSL_Frame;
//! Flag de Mensaje entrante Completo.
static volatile unsigned char Modbus_OSL_Processing_Flag;
//! Flag de Reenvío de Mensaje.
static unsigned char Modbus_OSL_Forward_Flag;
//! Almacena el Nº de envíos que lleva el mensaje actual.
static unsigned char Modbus_OSL_Attempt;
//! \brief Nº Máximo de envíos para un mensaje, si se alcanza y se sigue sin 
//! recibir una respuesta, se descarta el mensaje y se pasa a los siguientes.
static unsigned char Modbus_OSL_Max_Attempts;
//! Variable que almacena el Nº de Slave del que se espera la respuesta.
static unsigned char Modbus_OSL_Expected_Slave;
//! Vector para almacenar los mensajes de Salida del Master.
static unsigned char Modbus_OSL_Req_ADU[256];
//! Longitud del mensaje de Salida del Master.
static unsigned char Modbus_OSL_L_Req_ADU;

// Para los distintos estados de los diagramas de Master y RTU.

//! Estado del Sistema en el diagrama de Master.
static volatile enum Modbus_OSL_MainStates Modbus_OSL_MainState;
//! Estado del Sistema en el diagrama RTU o ASCII.
static volatile enum Modbus_OSL_States Modbus_OSL_State;
//! @}

//*****************************************************************************
//
// Prototipos de las funciones locales del módulo OSL.
//
//*****************************************************************************

static void Modbus_OSL_Set_Timeout_B (uint32_t Baudrate);
static void Modbus_OSL_Set_Timeout_R (uint32_t Baudrate);
static void Modbus_OSL_Response_Timeout(void);
static void Modbus_OSL_BroadCast_Timeout(void);
void Modbus_OSL_Repeat_Request (void);
unsigned char Modbus_OSL_Resend(void);
static unsigned char Modbus_OSL_Processing_Msg(void);
static void Modbus_OSL_RTU_to_App (void);
static void Modbus_OSL_Send (unsigned char *mb_req_pdu, unsigned char L_pdu);

//*****************************************************************************
//! \defgroup OSL_Var Gestión de Variables 
//! \ingroup OSL
//! \brief Funciones para consultar/modificar variables desde otros módulos. 
//!
//! Los módulos App y OSL_RTU necesitan en algunos casos conocer o modificar 
//! los valores de ciertas variables del Módulo OSL, como los Estados del 
//! Sistema, el Baudrate, la corrección de la Trama o el flag de Broadcast. Las
//! llamada a las siguientes funciones lo permite sin externalizar dichas
//! variables.
//*****************************************************************************
//! @{

//! \brief Obtiene el Baudrate del Sistema.
//!
//! \return Modbus_OSL_Baudrate Baudrate del Sistema
//! \sa Modbus_OSL_Baudrate, enum Baud
uint32_t Modbus_OSL_Get_Baudrate(void)
{
  return(Modbus_OSL_Baudrate);
}

//! \brief Obtiene el Estado de corrección de la trama entrante.
//!
//! El mensaje entrante tiene marcado en _Modbus_OSL_Frame_ si la trama 
//! recibida es correcta o bien se ha detectado algún error, bien sea por
//! paridad, exceso de caracteres o error en el CRC; esta función permite
//! conocer dicho estado.
//! \return Modbus_OSL_Frame Puede ser MODBUS_OSL_Frame_OK/MODBUS_OSL_Frame_NOK
//! \sa Modbus_OSL_Frame, Modbus_OSL_Frame_Set, enum Modbus_OSL_Frames
enum Modbus_OSL_Frames Modbus_OSL_Frame_Get(void)
{
  return Modbus_OSL_Frame;
}

//! \brief Fija el Estado de corrección de la trama entrante.
//!
//! El mensaje entrante tiene marcado en _Modbus_OSL_Frame_ si la trama 
//! recibida es correcta o bien se ha detectado algún error, bien sea por
//! paridad, exceso de caracteres o error en el CRC; esta función permite
//! marcar el valor de dicho estado para modificarlo desde otro módulo.
//! \param Flag Puede ser MODBUS_OSL_Frame_OK/MODBUS_OSL_Frame_NOK
//! \sa Modbus_OSL_Frame, Modbus_OSL_Frame_Get, enum Modbus_OSL_Frames
void Modbus_OSL_Frame_Set(enum Modbus_OSL_Frames Flag)
{
  Modbus_OSL_Frame = Flag;
}

//! \brief Obtiene el Estado del diagrama RTU/ASCII.
//!
//! Esta función permite conocer el estado del diagrama de comunicaciones Serie
//! RTU o ASCII, dependiendo de que modo de comunicaciones serie se esté usando.
//! \return Modbus_OSL_State Estado del Diagrama de Estados RTU/ASCII
//! \sa Modbus_OSL_State, Modbus_OSL_State_Set, enum Modbus_OSL_States 
enum Modbus_OSL_States Modbus_OSL_State_Get(void)
{
  return Modbus_OSL_State;
}

//! \brief Cambia el Estado del diagrama RTU/ASCII.
//!
//! Esta función permite fijar o cambiar el estado del diagrama de 
//! comunicaciones Serie RTU o ASCII, dependiendo del modo que se esté usando. 
//! \param State Estado del Diagrama de Estados RTU/ASCII a escribir
//! \sa Modbus_OSL_State, Modbus_OSL_State_Get, enum Modbus_OSL_States
void Modbus_OSL_State_Set(enum Modbus_OSL_States State)
{
  Modbus_OSL_State = State;
}

//! \brief Obtiene el Estado del diagrama del Master.
//!
//! \return Modbus_OSL_MainState Estado del Diagrama de Estados del Master
//! \sa Modbus_OSL_MainState, Modbus_OSL_MainState_Set, enum Modbus_OSL_MainStates 
enum Modbus_OSL_MainStates Modbus_OSL_MainState_Get (void)
{
   return Modbus_OSL_MainState;
}

//! \brief Cambia el Estado del diagrama del Master.
//!
//! Esta función permite fijar o cambiar el estado del diagrama de 
//! comunicaciones Serie del Master. 
//! \param State Estado del Diagrama de Estados del Master a escribir
//! \sa Modbus_OSL_MainState, Modbus_OSL_MainState_Get, enum Modbus_OSL_MainStates
void Modbus_OSL_MainState_Set (enum Modbus_OSL_MainStates State)
{
  Modbus_OSL_MainState = State;
}
//! @}

//*****************************************************************************
//! \defgroup OSL_Manage Gestión de comunicaciones Serie
//! \ingroup OSL
//! \brief Funciones para la configuración y manejo de las comunicaciones Serie. 
//!
//! Las funciones siguientes se encargan tanto de configurar el sistema para 
//! comunicaciones por puerto Serie, gestionando la interrupción de la UART1
//! usada en esas comunicaciones, como de Gestionar la Recepción/Envío de los
//! mensajes siguiendo el diagrama de comportamiento del Master de las
//! especificaciones del protocolo Modbus sobre puerto Serie.
//! ![Diagrama de Comportamiento del Master](../../Master.png 
//! "Diagrama de Comportamiento del Master")
//*****************************************************************************
//! @{

//! \brief Establece el Nº de cuentas para el Timeout de Respuesta.
//!
//! En función del Baudrate de las comunicaciones Serie almacena en 
//! _Modbus_OSL_Timeout_R_ el Nº de cuentas necesario para establecer un tiempo
//! de desborde en un timer considerado suficiente para que un Slave reciba y
//! procese una petición y se reciba la respuesta. Por ejemplo: 1s a 9600bps.
//! \param Baudrate Baudrate de las comunicaciones Serie
//! \sa Modbus_OSL_Timeout_R, Modbus_OSL_Response_Timeout, Modbus_OSL_Timeouts
void Modbus_OSL_Set_Timeout_R (uint32_t Baudrate)
{ 
  switch (Baudrate) 
  { 
    // 4 segundos. 
    case (1200):
      Modbus_OSL_Timeout_R=SysCtlClockGet()*4;
      break;
    // 3 segundos.
    case (2400):
      Modbus_OSL_Timeout_R=SysCtlClockGet()*3;
      break;
    // 2 segundos.
    case (4800):
      Modbus_OSL_Timeout_R=SysCtlClockGet()*2;
      break;
    // 1 segundo.
    case (9600):
      Modbus_OSL_Timeout_R=SysCtlClockGet();
      break;
    // 0,5 segundos.  
    default:
      Modbus_OSL_Timeout_R=SysCtlClockGet()/2;
      break;    
  }
}

//! \brief Establece el Nº de cuentas para el Timeout de BroadCast.
//!
//! En función del Baudrate de las comunicaciones Serie almacena en 
//! _Modbus_OSL_Timeout_B_ el Nº de cuentas necesario para establecer un tiempo
//! de desborde en un timer considerado suficiente para que los Slaves reciban 
//! y procesen una petición BroadCast. Por ejemplo: 400ms a 9600bps.
//! \param Baudrate Baudrate de las comunicaciones Serie
//! \sa Modbus_OSL_Timeout_B, Modbus_OSL_BroadCast_Timeout, Modbus_OSL_Timeouts
void Modbus_OSL_Set_Timeout_B(uint32_t Baudrate)
{ 
  switch (Baudrate) 
  {
    // 2,5 segundos.
    case (1200):
      Modbus_OSL_Timeout_B=SysCtlClockGet()*5/2;
      break;
    // 1,5 segundos.
    case (2400):
      Modbus_OSL_Timeout_B=SysCtlClockGet()*3/2;
      break;
    // 800 ms.  
    case (4800):
      Modbus_OSL_Timeout_B=SysCtlClockGet()*4/5;
      break;
    // 400 ms.
    case (9600):
      Modbus_OSL_Timeout_B=SysCtlClockGet()*2/5;
      break;
    // 200 ms.  
    default:
      Modbus_OSL_Timeout_B=SysCtlClockGet()/5;
      break;    
  }
}

//! \brief Carga el Timer 2 para el Timeout de BroadCast y lo arranca.
//! 
//! Carga _Modbus_OSL_Timeout_B_ en el numero de cuentas del timer 2 y lo 
//! arranca, pasando al estado DELAY, de este modo, el sistema espera hasta
//! que salte el Timeout de Broadcast antes de volver a IDLE y seguir mandando
//! peticiones. Esta función se activa al enviar una petición en modo BroadCast.
//! \sa Modbus_OSL_Timeout_B, Modbus_OSL_Timeouts, Modbus_OSL_Output
void Modbus_OSL_BroadCast_Timeout(void)
{
   TimerLoadSet(TIMER2_BASE, TIMER_A, Modbus_OSL_Timeout_B);      
   TimerEnable(TIMER2_BASE, TIMER_A);
   Modbus_OSL_MainState=MODBUS_OSL_DELAY;
}

//! \brief Carga el Timer 2 para el Timeout de Respuesta y lo arranca.
//! 
//! Carga _Modbus_OSL_Timeout_R_ en el numero de cuentas del timer 2 y lo 
//! arranca, pasando al estado WAITREPLY, de este modo, el sistema no espera 
//! indefinidamente una respuesta y si no la recibe y salta el Timeout de 
//! Respuesta, reenvía la petición hasta el Nº Máximo de envíos. Esta función 
//! se activa al enviar una petición en modo Unicast.
//! \sa Modbus_OSL_Timeout_R, Modbus_OSL_Timeouts, Modbus_OSL_Output
void Modbus_OSL_Response_Timeout(void)
{
   TimerLoadSet(TIMER2_BASE, TIMER_A, Modbus_OSL_Timeout_R);      
   TimerEnable(TIMER2_BASE, TIMER_A);
   Modbus_OSL_MainState=MODBUS_OSL_WAITREPLY;
}

//! \brief Función para la interrupción de Timeout de BroadCast/Respuesta.
//! 
//! En función del Estado del Master realiza las acciones del Timeout adecuado. 
//! Si esta en _MODBUS_OSL_DELAY_ (BroadCast), vuelve al estado IDLE para seguir
//! enviando mensajes, puesto que no se espera ninguna respuesta. Si el estado
//! es _MODBUS_OSL_WAITREPLY_ (Unicast), lo cambia a MODBUS_OSL_ERROR para que 
//! se active el reenvío de mensaje o se pase al siguiente, según convenga.
//! \sa Modbus_OSL_Response_Timeout, Modbus_OSL_BroadCast_Timeout
//! \sa Modbus_OSL_Output, Modbus_OSL_Serial_Comm
void Modbus_OSL_Timeouts(void)
{
  switch(Modbus_OSL_MainState_Get())
  {
    case MODBUS_OSL_WAITREPLY:
          Modbus_OSL_MainState_Set(MODBUS_OSL_ERROR);  
          break;
    case MODBUS_OSL_DELAY:
          Modbus_OSL_MainState_Set(MODBUS_OSL_IDLE);
          break;
    default:
          Modbus_Fatal_Error(110);
  }
}

//! \brief Configura las comunicaciones Serie.
//!
//! Establece el Nº de Envíos de un Mensaje que no reciba una respuesta
//! apropiada antes de descartarlo, el modo de comunicación RTU o ASCII y el 
//! Baudrate e inicia el Estado de Comportamiento, el numero actual de envíos y 
//! los flags de Reenvío, Sin Respuesta, Mensaje entrante y corrección de trama  
//! a sus valores iniciales. Configura la UART1 según modo RTU/ASCII para cumplir   
//! sus especificaciones y configura el LED1 para encenderlo al transmitir y
//! recibir datos. Configura el _Timer_ _2_ para crear una interrupción que se
//! usa como Timeout para Reenviar un Mensaje o para esperar que se procesen
//! las peticiones Broadcast antes de enviar nuevos mensajes (puesto que sólo se
//! puede enviar un mensaje por vez se usa el mismo Timer para ambos casos pero 
//! con distinto numero de cuentas), que además depende del Baudrate. Finalmente 
//! llama a la función de configuración e inicio del modo de comunicación RTU/ASCII.
//! \param Baudrate Baudrate con que iniciar las comunicaciones
//! \param Mode Modo de comunicación en Serie, RTU (por defecto) o ASCII
//! \param Attempts Nº de intentos de Envío antes de descartar petición
//! \sa enum Baud, enum Modbus_OSL_Modes, Modbus_OSL_RTU_Init
void Modbus_OSL_Init (enum Baud Baudrate, enum Modbus_OSL_Modes Mode,unsigned char Attempts)
{
    Modbus_OSL_Processing_Flag=0;
    Modbus_OSL_Forward_Flag=0;
    Modbus_OSL_Max_Attempts=Attempts;
    Modbus_OSL_Attempt=1;
    Modbus_OSL_Frame_Set(MODBUS_OSL_Frame_OK);
    
    
    if (Baudrate == BDEFAULT) 
        Modbus_OSL_Baudrate=B19200;
    else
      Modbus_OSL_Baudrate=Baudrate;
    
    Modbus_OSL_MainState=MODBUS_OSL_INITIAL;
    
    if (Mode == MDEFAULT || Mode == MODBUS_OSL_MODE_RTU) 
    {
      Modbus_OSL_Mode=MODBUS_OSL_MODE_RTU;
    }
    else
    {
      Modbus_OSL_Mode=MODBUS_OSL_MODE_ASCII;    
    }
    
    // Habilita los periféricos de la UART y los pins usados para las
    // comunicaciones. Se utiliza la UART1, que requiere pins del puerto GPIOD.
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART1);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    
    // Habilita el Timer 2
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER2);
    
    // Habilita las interrupciones del sistema.
    IntMasterEnable();
    
    // Fija GPIO D2 y D3 como los pins de la UART1.
    GPIOPinTypeUART(GPIO_PORTD_BASE, GPIO_PIN_2 | GPIO_PIN_3);  
    
    switch(Modbus_OSL_Mode)
    {
      case MODBUS_OSL_MODE_RTU:
        // Configura la UART para el Baudrate, 8-Par-1.
        UARTConfigSetExpClk(UART1_BASE, SysCtlClockGet(), Modbus_OSL_Baudrate,
                           (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                            UART_CONFIG_PAR_EVEN));
        break;
      case MODBUS_OSL_MODE_ASCII:
        // Configura la UART para el Baudrate, 7-Par-1.
        UARTConfigSetExpClk(UART1_BASE, SysCtlClockGet(), Modbus_OSL_Baudrate,
                           (UART_CONFIG_WLEN_7 | UART_CONFIG_STOP_ONE |
                            UART_CONFIG_PAR_EVEN));
        break;
    }
    
    // Desactiva la cola FIFO de la UART para que las interrupciones salten por
    // cada carácter recibido.
    UARTFIFODisable(UART1_BASE);
    
    // Habilita el puerto GPIO usado para el LED1.
    SYSCTL_RCGC2_R = SYSCTL_RCGC2_GPIOF;
    // Lectura aleatoria para fijar unos pocos ciclos al activar el periférico.
    volatile unsigned long ulLoop = SYSCTL_RCGC2_R;
    // Habilita el pin GPIO para el LED (PF0) y fija la dirección como salida.
    GPIO_PORTF_DIR_R = 0x01;
    GPIO_PORTF_DEN_R = 0x01;
    
    // Configura el Timer 2 como de 32-bits y Establece las cuentas de los
    // Timeouts de Respuesta y de BroadCast.
    TimerConfigure(TIMER2_BASE, TIMER_CFG_ONE_SHOT);
    Modbus_OSL_Set_Timeout_B (Modbus_OSL_Baudrate);
    Modbus_OSL_Set_Timeout_R (Modbus_OSL_Baudrate);
    
    // Habilita la interrupción de la UART, para Recepción y error de paridad.
    UARTIntEnable(UART1_BASE, UART_INT_RX | UART_INT_PE);
    IntEnable(INT_UART1);
    
    // Activa la Interrupción por desborde del Timer 2.
    IntEnable(INT_TIMER2A);
    TimerIntEnable(TIMER2_BASE, TIMER_TIMA_TIMEOUT);
    
    switch(Modbus_OSL_Mode)
    {
      case MODBUS_OSL_MODE_RTU:
        Modbus_OSL_RTU_Init();
        break;
      case MODBUS_OSL_MODE_ASCII:
        break;
    }
}

//! \brief Interrupción por Recepción de Carácter.
//! 
//! Esta función se activa con la interrupción de la UART1. Enciende el LED1 
//! para indicar que se esta produciendo la comunicación, limpia el status de
//! la interrupción y se asegura de que se esté en estado de esperar respuesta. 
//! Si es así comprueba si es de error de paridad para marcar la trama como NOK;
//! en caso contrario llama a la función de interrupción RTU/ASCII que 
//! corresponda según el modo de comunicación Serie.
//!
//! __NOTA__:También se aceptan caracteres en estado ERROR por si salta la
//! interrupción de Respuesta mientras se está recibiendo un mensaje para acabar
//! de recibirlo. Como el estado es ERROR el mensaje será descartado igualmente.
//! \sa Modbus_OSL_Frame_Set, Modbus_OSL_Mode, Modbus_OSL_RTU_UART
void UART1IntHandler(void)
{
    unsigned long ulStatus;
    
    // Enciende el Led1.
    GPIO_PORTF_DATA_R |= 0x01;        
    
    // Obtiene el estado de la interrupción y lo borra.
    ulStatus = UARTIntStatus(UART1_BASE, true);
    UARTIntClear(UART1_BASE, ulStatus);
   
    // Si el estado no es WAITREPLY o ERROR descarta el caracter.
    if(Modbus_OSL_MainState_Get()==MODBUS_OSL_WAITREPLY 
       || Modbus_OSL_MainState_Get()==MODBUS_OSL_ERROR)
    {
      // Si el estado de la interrupción es UART_INT_PE (por error de paridad)
      // marca la trama como NOK; Si no, llama a la función correspondiente.
      if (UART_INT_PE==ulStatus)
      {
        Modbus_OSL_Frame_Set(MODBUS_OSL_Frame_NOK);     
      }
      else
      {
        //Debug_OSL_IncChar++;
        switch (Modbus_OSL_Mode)
        {
          case MODBUS_OSL_MODE_RTU:
              Modbus_OSL_RTU_UART();
              break;
                
          case MODBUS_OSL_MODE_ASCII:
              break;
            
          default:
              Modbus_Fatal_Error(100);
        }
      }
    }
    else
    {
        UARTCharGetNonBlocking(UART1_BASE);
    }
    
    // Apaga el LED1.
    GPIO_PORTF_DATA_R &= ~(0x01); 
}

//! \brief Implementación práctica del Diagrama de Comportamiento del Master.
//! 
//! Para seguir el esquema de comportamiento del Master realiza las siguientes
//! acciones en función del estado
//! > - __MODBUS_OSL_IDLE__: Si el flag de reenvío esta activo lo borra y reenvía
//! >     el mensaje actual; si no, desencola un mensaje de la cola FIFO y lo
//! >     envía a menos que ya no queden mensajes pendientes.
//! > - __MODBUS_OSL_WAITREPLY__: Si se recibe una trama de mensaje correcta del
//! >     Slave esperado se procesa el mensaje. Si este es una respuesta de 
//! >     excepción o hay algún error en los datos pasa a _MODBUS_OSL_ERROR_ y 
//! >     lo gestiona, volviendo al estado IDLE o permaneciendo en ERROR para
//! >     activar el flag de Reenvío. Si la respuesta es correcta vuelve a IDLE.
//! > - __MODBUS_OSL_ERROR__: Activa el Flag de reenvío si el numero de envíos
//! >     no excede el máximo
//! Cabe destacar que si el timeout de respuesta salta antes de recibirla se 
//! pasa directamente al estado de ERROR, lo que activará el flag de reenvío. 
//! Por otro lado si la petición es de Broadcast no se espera respuesta, luego 
//! se espera al timeout de BroadCast y se vuelve a IDLE.
//! \return 1 Se están procesando las comunicaciones
//! \return 0 No queda ninguna comunicación que realizar, no hay peticiones
//! \sa Modbus_OSL_Resend, Modbus_App_Send, Modbus_App_FIFOSend
//! \sa Modbus_OSL_Receive_CallBack, Modbus_App_Manage_CallBack 
//! \sa Modbus_OSL_Repeat_Request, enum Modbus_OSL_MainStates
unsigned char Modbus_OSL_Serial_Comm (void)
{
  switch(Modbus_OSL_MainState_Get())
  {
    case MODBUS_OSL_IDLE:
        // Si el flag de reenvío está activado, se envía de nuevo el mensaje.
        if(Modbus_OSL_Resend())
        {
          //Debug_OSL_Rsp_Resend++;
          Modbus_App_Send();
        }
        else
        {
          // Si no hay reenvío y quedan mensajes en la cola FIFO se desencola
          // y envía la siguiente petición. Si no hay mensajes devuelve 0.
          if(Modbus_App_FIFOSend())
              return 0;
        }
         break;
        
    case MODBUS_OSL_WAITREPLY:
        // Si hay un mensaje entrante correcto se procesa la respuesta.
        if (Modbus_OSL_Receive_CallBack()) 
          Modbus_App_Manage_CallBack();
        break;
 
    case MODBUS_OSL_ERROR:
        // Se espera (sin detener el programa) a que cese la recepción de
        // mensajes si está activa.
        if (Modbus_OSL_State_Get()==MODBUS_OSL_RTU_IDLE || 
	   Modbus_OSL_State_Get()==MODBUS_OSL_ASCII_IDLE)    
        {
          // Si no se recibe una respuesta correcta, activa Flag de Reenvío 
          // hasta el Nº máximo de envíos permitido y vuelve a MODBUS_OSL_IDLE;
          Modbus_OSL_Repeat_Request ();
          Modbus_OSL_MainState_Set(MODBUS_OSL_IDLE);
        }
        break;        
        
    default:
        // En otro estado, como MODBUS_OSL_DELAY, no hacer nada.
        break;
  }
  return 1;
}

//! \brief Leer y borrar el Flag de Reenvío.
//! 
//! Devuelve el valor de _Modbus_OSL_Forward_Flag_ y lo borra para que el 
//! Flag de Reenvío esté activo sólo 1 vez por activación. 
//! \return  Devuelve 0/1 en función del estado del Flag
//! \sa Modbus_OSL_Serial_Comm, Modbus_OSL_Repeat_Request
unsigned char Modbus_OSL_Resend(void) 
{
   unsigned char res;
   
   res = Modbus_OSL_Forward_Flag;
   Modbus_OSL_Forward_Flag = 0;
   return res;
}

//! \brief Activar el flag de Reenvío.
//! 
//! Si el Nº de Envíos no supera el Máximo, activa el Flag de Reenvío y aumenta
//! la cuenta de intentos de envío de un mensaje _Modbus_OSL_Attempt_ en uno.
//! Si se ha superado el numero de intentos resetea la cuenta a uno y llama a
//! _Modbus_App_No_Response_ para que encole en la cola de excepciones que se
//! ha ignorado un mensaje por no recibir respuesta.
//! \sa Modbus_OSL_Serial_Comm, Modbus_App_No_Response
void Modbus_OSL_Repeat_Request (void)
{
  if(Modbus_OSL_Attempt<Modbus_OSL_Max_Attempts)
  {
    Modbus_OSL_Attempt++;
    Modbus_OSL_Forward_Flag=1;
  }
  else
  {
    Modbus_App_No_Response();
    Modbus_OSL_Attempt=1;               
  }
}

//! \brief Resetea la cuenta de Intentos de envío de un Mensaje.
//! 
//! \sa Modbus_OSL_Attempt, Modbus_App_Manage_CallBack
void Modbus_OSL__Attempt (void)
{
  Modbus_OSL_Attempt=1;
}

//! \brief Error Inesperado del Programa.
//!
//! Por Seguridad y robustez de la programación se incluye esta función que
//! detiene el proceso del programa si se da alguna posibilidad en principio
//! excluida en la implementación y considerada imposible. Esto también puede
//! pasar por detenciones e inicios abruptos e inesperados en el proceso o
//! defectos/averías de Hardware. En circunstancias normales es de suponer que
//! nunca se llega a este punto.
//!
//! Esta función está en OSL por estar relacionado éste módulo con OSL y App,
//! evitando tener que duplicar la función para errores en dichos módulos.
//! Además en su llamada incluye un parámetro con distintos valores en función
//! de donde salta el error.
//! \param Error Adopta un valor distinto para localizar la procedencia.
//! > - _Error_ = 10: En _Modbus_App_Manage_CallBack_ se llega a un Nº de función 
//! >    desconocido después de haberlo comprobado como conocido con anterioridad.
//! > - _Error_ = 20: En _Modbus_App_Send_ se pide enviar una función no 
//! >    implementada.
//! > - _Error_ = 100: Se llega a la interrupción de la UART sin determinar el 
//! >    modo de la conexión Serie.
//! > - _Error_ = 110: Se llega a _Modbus_OSL_Timeouts_ en la interrupción del 
//! >    Timer2 sin estar en _MODBUS_OSL_WAITREPLY_ o _MODBUS_OSL_DELAY_.
//! > - _Error_ = 200: Interrupción 1,5T en un estado donde no deberia poder 
//! >    activarse.
//! > - _Error_ = 210: Interrupción 3,5T en un estado donde no debería poder
//! >    activarse.
//! \sa UART1IntHandler, Modbus_OSL_RTU_15T, Modbus_OSL_RTU_35T
//! \sa Modbus_App_Manage_CallBack, Modbus_App_Send, Modbus_OSL_Timeouts
void Modbus_Fatal_Error(unsigned char Error)
{  
  while(1)
  {     

  }
}
//! @}

//*****************************************************************************
//! \defgroup OSL_Input Entrada de Mensajes
//! \ingroup OSL_Manage
//! \brief Funciones para la gestión de la entrada de mensajes. 
//!
//! Funciones encargadas de detectar y procesar los mensajes entrantes una vez
//! completos para, si son correctos, enviar la información al módulo de 
//! aplicación Modbus App. Cabe destacar que la comprobación del Nº de Slave se
//! realiza en esta parte; de modo que Modbus App sólo debe ocuparse de la
//! información referente a las funciones de Usuario de Modbus.
//*****************************************************************************
//! @{

//! \brief Activa el Flag de Mensaje Completo Recibido.
//!
//! Si el estado no es _MODBUS_OSL_WAITREPLY_ sólo puede ser porque ha saltado
//! el Timeout de respuesta (_Modbus_OSL_Timeouts_) durante la reconfiguración
//! de los punteros en _Modbus_OSL_RTU_35T_ en el caso CONTROLANDWAITING del
//! switch; puesto que el mensaje no ha sido aun procesado se descarta y se
//! reenviará la petición; por robustez de la programación.
//! \sa Modbus_OSL_Processing_Flag,Modbus_OSL_RTU_35T
//! \sa Modbus_OSL_Processing_Msg, Modbus_OSL_Timeouts
void Modbus_OSL_Reception_Complete(void)
{
  if(Modbus_OSL_MainState_Get()==MODBUS_OSL_WAITREPLY)
      Modbus_OSL_Processing_Flag = 1;
}

//! \brief Leer y borrar el Flag de Mensaje Completo Recibido.
//! 
//! Devuelve el valor de _Modbus_OSL_Processing_Flag_ y lo borra para que el 
//! Flag de Mensaje entrante esté activo sólo 1 vez por activación. Deshabilita
//! las interrupciones durante el proceso para evitar una posible activación del 
//! Flag durante el propio proceso, perdiendo un mensaje entrante. 
//! \return  Devuelve 0/1 en función del estado del Flag
//! \sa Modbus_OSL_Processing_Flag, Modbus_OSL_Receive_CallBack
static unsigned char Modbus_OSL_Processing_Msg(void) 
{
   unsigned char res;
   IntMasterDisable();
   res = Modbus_OSL_Processing_Flag;
   Modbus_OSL_Processing_Flag = 0;
   IntMasterEnable();
   return res;
}

//! \brief Envía un Mensaje entrante Correcto a Modbus App.
//! 
//! Cuando se ha comprobado completamente la corrección de un mensaje entrante
//! se envía al módulo Modbus App para procesar la información contenida. Se
//! utilizan las funciones _Modbus_App_Receive_Char_ y _Modbus_OSL_RTU_Char_Get_ 
//! para evitar la necesidad de guardar una copia del mensaje en el propio 
//! Módulo OSL. Del mismo modo se envía también la longitud del mensaje enviado
//! a App (no la longitud original del mensaje, sin CRC ni Nº de Slave).
//! \sa Modbus_App_Receive_Char, Modbus_OSL_RTU_Char_Get
//! \sa Modbus_App_L_Msg_Set, Modbus_OSL_RTU_L_Msg_Get 
static void Modbus_OSL_RTU_to_App (void)
{
  unsigned char i;
  
  // El primer carácter no se envía por ser el Nº Slave, ademas, por éste motivo
  // se disminuye la longitud del mensaje en 1. El CRC ya ha sido considerado. 
  for(i=1;i<Modbus_OSL_RTU_L_Msg_Get();i++)
      Modbus_App_Receive_Char (Modbus_OSL_RTU_Char_Get(i),i-1);
  Modbus_App_L_Msg_Set(Modbus_OSL_RTU_L_Msg_Get()-1);
}

//! \brief Leer Mensaje Entrante Completo.
//!
//! Si existe un mensaje entrante completo se comprueba el Nº Slave para saber
//! si debe procesarse. De ser así se comprueba el CRC y si es correcto se 
//! envía a App para su procesado mediante _Modbus_OSL_RTU_Control_CRC_ y se
//! vuelve al estado _MODBUS_OSL_IDLE_ para seguir recibiendo mensajes.

//! return 1 Un mensaje completo correcto ha sido enviado a App para su Lectura
//! return 0 No hay mensaje o Ignorar mensaje incorrecto.
//! \sa Modbus_OSL_Processing_Msg, Modbus_OSL_RTU_Char_Get
//! \sa Modbus_OSL_RTU_Control_CRC 
unsigned char Modbus_OSL_Receive_CallBack(void) 
{ 
  unsigned char Modbus_OSL_Slave;
  
   // Si hay un mensaje entrante completo.
   if (Modbus_OSL_Processing_Msg()) 
   {     
      switch (Modbus_OSL_Mode) 
      {
	case MODBUS_OSL_MODE_RTU:
              // Recibir numero de Slave.
              Modbus_OSL_Slave=Modbus_OSL_RTU_Char_Get(0);
              break;

        case MODBUS_OSL_MODE_ASCII:
              // Recibir numero de Slave.
              break;
      }
      // Comprobar si la respuesta es del Slave esperado.
      if(Modbus_OSL_Slave==Modbus_OSL_Expected_Slave)
      {
        //Debug_OSL_IncMsg++;
        // Se acepta el mensaje, así que se para el Timer 2 para evitar el
        // Timeout de Respuesta.
        TimerDisable(TIMER2_BASE, TIMER_A);
        // Se pasa al estado PROCESSING
        Modbus_OSL_MainState_Set(MODBUS_OSL_PROCESSING);
        // Comprobar CRC/LRC y enviar información a App si es correcto.
        switch (Modbus_OSL_Mode) 
        {
            case MODBUS_OSL_MODE_RTU:
                  
                if(Modbus_OSL_RTU_Control_CRC())
                {  
                  //Debug_OSL_CRC_OK++;
                  Modbus_OSL_RTU_to_App();
                  return 1;
                }
                else
                {
                  /* Si se descarta el mensaje por CRC volver la comprobación de
                  trama a OK para no descartar siguientes mensajes y volver a IDLE.*/
                  Modbus_OSL_Frame_Set(MODBUS_OSL_Frame_OK);
                  Modbus_OSL_MainState_Set(MODBUS_OSL_ERROR);
                }
                break;

            case MODBUS_OSL_MODE_ASCII:
                // Comprobar corrección y enviar a App el mensaje ASCII 
                // traducido a formato RTU.
                break;
        }    
      }
   }
    return 0;
}


//! @}

//*****************************************************************************
//! \defgroup OSL_Output Salida de Mensajes
//! \ingroup OSL_Manage
//! \brief Funciones para la gestión de la Salida de mensajes. 
//!
//! Funciones encargadas de la salida de mensajes en las comunicaciones Serie. 
//! Se recoge la trama PDU saliente del Módulo App y se le añade el numero de 
//! Slave al principio y el CRC/LRC al final para formar el ADU que se enviará.
//*****************************************************************************
//! @{

//! \brief Monta y envía el Mensaje.
//!
//! Del módulo App llega la información perteneciente a la función de usuario
//! de Modbus, bien sea de petición o de respuesta. Se le añaden el Nº de Slave
//! y el CRC mediante _Modbus_OSL_RTU_Mount_ADU_ (en caso de Modo ASCII se 
//! deberá implementar la adición del LRC y la traducción del formato) y se 
//! envia el mensaje mediante _Modbus_OSL_Send_. Se configura y se activa el 
//! Timer 2 en función de si es una petición a un Slave (Unicast) o una petición
//! BroadCast para activar el Timeout pertinente.
//! \param *mb_req_pdu Puntero al vector con el Mensaje de Salida de App (PDU)
//! \param Slave Nº de Slave de la petición.
//! \param L_pdu Longitud del Mensaje de Salida de App
//! \sa Modbus_App_Send, Modbus_OSL_RTU_Mount_ADU, Modbus_OSL_L_Req_ADU
//! \sa Modbus_OSL_Send, Modbus_OSL_BroadCast_Timeout, Modbus_OSL_Response_Timeout 
void Modbus_OSL_Output (unsigned char *mb_req_pdu, unsigned char Slave, unsigned char L_pdu)
{ 
  switch (Modbus_OSL_Mode) 
  {
      case MODBUS_OSL_MODE_RTU:
              // Montar ADU la longitud aumenta en 3 caracteres por el Slave y el CRC.
              // Pasa al estado Emission para cumplir el diagrama de estados de RTU.
              Modbus_OSL_RTU_Mount_ADU (mb_req_pdu,Slave,L_pdu,Modbus_OSL_Req_ADU);
              Modbus_OSL_L_Req_ADU=L_pdu+3;
              Modbus_OSL_State_Set(MODBUS_OSL_RTU_EMISSION);
          break;

      case MODBUS_OSL_MODE_ASCII:
          // Montar ADU, traducir a ASCII
          break;
  }    
  // Guardar el Nº de Slave al que se realiza la petición para sólo comprobar
  // las respuestas que vengan de dicho Slave y enviar.
  Modbus_OSL_Expected_Slave=Slave;
  Modbus_OSL_Send(Modbus_OSL_Req_ADU, Modbus_OSL_L_Req_ADU);
  
  if (Modbus_OSL_Mode==MODBUS_OSL_MODE_RTU)
  {
    // En RTU se activa el Timer 0 para volver a IDLE cuando desborde.
    TimerLoadSet(TIMER0_BASE, TIMER_A, Modbus_OSL_RTU_Get_Timeout_35());
    TimerEnable(TIMER0_BASE, TIMER_A); 
  }
 
  // Si la petición es de BroadCast
  if(Modbus_OSL_Expected_Slave==0)
  {
    // Iniciar Timer 2 para Timeout de BroadCast.
    Modbus_OSL_BroadCast_Timeout();
  }
  else
  {
    // Iniciar Timer 2 para Timeout de Respuesta.
    Modbus_OSL_Response_Timeout();
  }
}

//! \brief Función de Envío de Mensaje.
//!
//! Enciende el LED1 de comunicaciones y envía secuencialmente el número de 
//! caracteres indicado del vector señalado en los parámetros. Al terminar 
//! apaga el LED de comunicaciones.
//! \param *mb_req_adu Puntero al vector con el Mensaje de Salida completo(ADU)
//! \param L_adu Longitud del Mensaje de Salida Completo.
//! \sa Modbus_OSL_Output
static void Modbus_OSL_Send (unsigned char *mb_req_adu, unsigned char L_adu)
{
  char i;

  // Enciende el LED1.
  GPIO_PORTF_DATA_R |= 0x01;
  
  for (i=0;i<L_adu;i++)
  { 
    UARTCharPut(UART1_BASE,mb_req_adu[i]);
    //Debug_OSL_OutChar++;
  } 
  //Debug_OSL_OutMsg++;
  
  // Apaga el LED1.
  GPIO_PORTF_DATA_R &= ~(0x01);
}
//! @}
#endif