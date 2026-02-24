#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "vehicle_types.h"
#include "can.h"

/* =======================================
    DEFINIZIONI
   ======================================= */

// Dimensione pacchetto
#define MAX_LEN_DATA					8

// Bit masks
#define BIT_MASK                        0x01
#define DOUBLE_BIT_MASK                 0x03
#define HALF_BYTE_MASK                  0x0F

// VMS_data_1 byte 5 bit shifts
#define READY_LAMP_FLAG_SHIFT           1
#define RUN_OFF_FLAG_SHIFT              3
#define SIDE_STAND_FLAG_SHIFT           4
#define VMS_DSB_MODALITY_SHIFT          5

// VMS_data_1 byte 6 bit shifts
#define FLAG_LIMITION_SHIFT             4
#define ALARM_LAMP_SHIFT                5
#define TEMP_LAMP_SHIFT                 6
#define NUM_BATT_SHIFT                  7

//VMS_data_2 byte 2 bit shifts
#define BUZZER_STATUS_SHIFT             3
#define MIL_LAMP_SHIFT                  4

//DSB_data_1 byte 1 bit shifts
#define SHARING_ENABLE_SHIFT            2
#define BUZZER_REQUEST_SHIFT            3
#define FRAME_COUNTER_DSB_SHIFT         4

//DSB_icons bit shifts
#define TURNS_SHIFT                     1
#define LOW_BEAM_SHIFT                  2
#define CRUISE_CONTROL_SHIFT            3
#define BLUETOOTH_SHIFT                 4


/* =======================================
    PROTOTIPI
   ======================================= */

static uint8_t can_msg_decoder(struct can_frame *c, vehicle_t * p, int * cs);   // In base all'id del messaggio chiama il decoder relativo
static uint8_t VMS_data_1_decoder(struct can_frame *c, vehicle_t * p);          // Decoder VMS 1 (Marcia)
static uint8_t VMS_data_2_decoder(struct can_frame *c, vehicle_t * p);          // Decoder VMS 2 (Dati secondari: Batteria...)
static uint8_t DSB_data_1_decoder(struct can_frame *c, vehicle_t * p);          // Decoder DSB 1 (Informazioni da cruscotto a centralina: Modalità...)
static uint8_t DSB_icons_decoder(struct can_frame *c, vehicle_t * p);           // Decoder DSB icone (Tutte le possibili icone: motore, abbaglianti...)
static void rx_mutex_cleanup_fn(void *arg);                                     // Cleanup handler per thread can_rx_handler, permette di rilasciare il mutex in tutti i casi

/* =======================================
    IMPLEMENTAZIONI
   ======================================= */

static void rx_mutex_cleanup_fn(void *arg)
{
    pthread_mutex_unlock((pthread_mutex_t *)arg);
}

/* ---------------------------------------------------------------
    TIPI DECODER
   --------------------------------------------------------------- */

static uint8_t VMS_data_1_decoder(struct can_frame *c, vehicle_t * p)
{

    // Vehicle speed to be displayed on DSB
    memcpy(&p->VMSData1.Speed_DBS, &c->data[0], sizeof(p->VMSData1.Speed_DBS));
    p->VMSData1.Speed_DBS = p->VMSData1.Speed_DBS * 0.5;
    DEBUG_PRINT("Speed: %u\n", p->VMSData1.Speed_DBS);

    //Energy consumption of the battery, calculated by VMS.
    memcpy(&p->VMSData1.Energy_Consumption, &c->data[1], sizeof(p->VMSData1.Energy_Consumption));
    DEBUG_PRINT("Energy consumption: %u\n", p->VMSData1.Energy_Consumption);

    // Byte 2 of CAN DLC is not used

    // Battery state of charge
    memcpy(&p->VMSData1.SOC_dsb, &c->data[3], sizeof(p->VMSData1.SOC_dsb));
    p->VMSData1.SOC_dsb = p->VMSData1.SOC_dsb * 0.4;
    DEBUG_PRINT("SOC_dsb: %u\n", p->VMSData1.SOC_dsb);

    // Byte 4 of CAN DLC is used as padding

    // Flag LowSOC shall activate the low SCO lamp. Flag LowSOC = 0: not active; Flag LowSOC = 1: active
    p->VMSData1.LowSOC = c->data[5] & BIT_MASK;
    DEBUG_PRINT("LowSOC: %d\n", p->VMSData1.LowSOC);

    // This flag is active when a modality has been selected and VMS is no longer in NO_MODE modality.
    p->VMSData1.Ready_Lamp = (c->data[5] >> READY_LAMP_FLAG_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Ready_Lamp: %d\n", p->VMSData1.Ready_Lamp);

    // Bit 3 fo byte 5 is used for padding

    // If this bit is set to 1, the RUN_OFF symbol shall be displayed on DSB.
    p->VMSData1.Run_off_VMS = (c->data[5] >> RUN_OFF_FLAG_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Run_off_VMS: %d\n", p->VMSData1.Run_off_VMS);

    // If this bit is set to 1, the SIDE_STAND symbol shall be displayed on DSB.
    p->VMSData1.Side_Stand = (c->data[5] >> SIDE_STAND_FLAG_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Side_Stand: %d\n", p->VMSData1.Side_Stand);

    // VMS and DSB Modality
    p->VMSData1.Modality_VMS_DSB = (c->data[5] >> VMS_DSB_MODALITY_SHIFT) & DOUBLE_BIT_MASK;
    DEBUG_PRINT("Modality_VMS_DSB: %u\n", p->VMSData1.Modality_VMS_DSB);

    // Bit 7 fo byte 5 is used for padding

    //This counter ensures to VMS that DSB communication on CAN bus is active.
    p->VMSData1.counter_VMS_to_DSB = c->data[6] & HALF_BYTE_MASK;
    DEBUG_PRINT("Counter_VMS_to_DSB: %u\n", p->VMSData1.counter_VMS_to_DSB);

    // Flag limitation shall be activated when a limitation is active that reduces significatively the actual torque.
    p->VMSData1.Flag_Limitation = (c->data[6] >> FLAG_LIMITION_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Flag_Limitation: %d\n", p->VMSData1.Flag_Limitation);

    // Alarm lamp request is active if Alarm_lamp = 1, not active if Alarm_lamp = 0.
    p->VMSData1.Alarm_lamp = (c->data[6] >> ALARM_LAMP_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Alarm_lamp: %d\n", p->VMSData1.Alarm_lamp);

    p->VMSData1.Flag_temp_lamp = (c->data[6] >> TEMP_LAMP_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Flag_temp_lamp: %d\n", p->VMSData1.Flag_temp_lamp);

    // Number of batteries
    p->VMSData1.Number_of_batteries = (c->data[6] >> NUM_BATT_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Number_of_batteries: %d\n", p->VMSData1.Number_of_batteries);

    // Range DSB
    memcpy(&p->VMSData1.Range_DSB, &c->data[7], sizeof(p->VMSData1.Range_DSB));
    DEBUG_PRINT("Range_DSB: %u\n", p->VMSData1.Range_DSB);

    return 0;
}

static uint8_t VMS_data_2_decoder(struct can_frame *c, vehicle_t * p)
{

    // Present used power given as a percentage of the maximum power allowable.
    memcpy(&p->VMSData2.Power_battery_percentage, &c->data[0], sizeof(p->VMSData2.Power_battery_percentage));
    p->VMSData2.Power_battery_percentage = p->VMSData2.Power_battery_percentage - 127;
    DEBUG_PRINT("Power_battery_percentage: %d\n", p->VMSData2.Power_battery_percentage);

    // byte 1 is used for padding

    // Buzzer lamp active if Buzzer_Status = 1, not active if Buzzer_Status = 0.
    p->VMSData2.Buzzer_Status = (c->data[2] >> BUZZER_STATUS_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Buzzer_Status: %d\n", p->VMSData2.Buzzer_Status);
    
    // MIL lamp active if MIL_lamp = 1, not active if MIL_lamp = 0.
    p->VMSData2.MIL_lamp = (c->data[2] >> MIL_LAMP_SHIFT) & BIT_MASK;
    DEBUG_PRINT("MIL_lamp: %d\n", p->VMSData2.MIL_lamp);

    // Byte 3 is used for padding

    // This flag gives the information that a key-off signal has been received and DCDC will shutdown soon.
    p->VMSData2.key_off = c->data[4] & BIT_MASK;
    DEBUG_PRINT("Key_off: %d\n", p->VMSData2.key_off);

    return 0;
}

static uint8_t DSB_data_1_decoder(struct can_frame *c, vehicle_t * p)
{

    // Byte 0 is used for padding

    // DSB sends to VMS the selected modality
    p->DBSData1.Modality_DSB_VMS = c->data[1] & DOUBLE_BIT_MASK;
    DEBUG_PRINT("Modality_DSB_VMS: %d\n", p->DBSData1.Modality_DSB_VMS );

    //sharing enable flag
    p->DBSData1.sharing_enable = (c->data[1] >> SHARING_ENABLE_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Sharing_enable: %d\n", p->DBSData1.sharing_enable );

    // Buzzer request flag
    p->DBSData1.Buzzer_request = (c->data[1] >> BUZZER_REQUEST_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Buzzer_request: %d\n", p->DBSData1.Buzzer_request );

    // This counter ensures to DSB that VMS communication on CAN bus is active.
    p->DBSData1.FrameCounterDSB = (c->data[1] >> FRAME_COUNTER_DSB_SHIFT) & HALF_BYTE_MASK;
    DEBUG_PRINT("FrameCounterDSB: %d\n", p->DBSData1.FrameCounterDSB );

    // It identifies the vehicle type based on homologation code
    memcpy(&p->DBSData1.Vehicle_Code, &c->data[5], sizeof(p->DBSData1.Vehicle_Code));
    DEBUG_PRINT("Vehicle_Code: %d\n", p->DBSData1.Vehicle_Code );

    // Allows to identify vehicle subtype in future projects
    memcpy(&p->DBSData1.Vehicle_Order, &c->data[6], sizeof(p->DBSData1.Vehicle_Order));
    DEBUG_PRINT("Vehicle_Order: %d\n", p->DBSData1.Vehicle_Order );

    return 0;
}

static uint8_t DSB_icons_decoder(struct can_frame *c, vehicle_t * p)
{
    static bool buzzer_old_state = 0;

    p->high_beam = c->data[0] & BIT_MASK;
    DEBUG_PRINT("High beam: %d\n", p->high_beam);

    p->low_beam = (c->data[0] >> LOW_BEAM_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Low beam: %d\n", p->low_beam);

    p->turns = (c->data[0] >> TURNS_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Turns: %d\n", p->turns);

    p->cruise_control = (c->data[0] >> CRUISE_CONTROL_SHIFT) & BIT_MASK;
    DEBUG_PRINT("Cruise control: %d\n", p->cruise_control);
    
    // Lo lascio ma non mi serve
    p->bleStatus = (c->data[0] >> BLUETOOTH_SHIFT) & BIT_MASK;

    return 0;
}

/* ---------------------------------------------------------------
    GESTIONE NUOVI ARRIVI
   --------------------------------------------------------------- */

static uint8_t can_msg_decoder(struct can_frame *c, vehicle_t * p, int * cs)
{
    uint8_t err;

	err = 0;
	switch (c->can_id)
	{
	case DSB_icons:
            err = DSB_icons_decoder(c,p);
	    break;
    case VMS_data_1_msg:
		    err = VMS_data_1_decoder(c, p);
		    p->idMessage = 0;
	    break;
	case VMS_data_2_msg:
            err = VMS_data_2_decoder(c, p);
            p->idMessage = 1;
	    break;
	case DSB_data_1_msg:
            err = DSB_data_1_decoder(c, p);
            p->idMessage = 2;
	    break;
	default:
		    err = 1;
	    break;
	}

	return err;
}

void * can_rx_handler (void * arg)
{
    my_struct_t * data = (my_struct_t *) arg;
    struct can_frame rxMsgFrame = {};
    ssize_t nBytesRead = 0;

    /* Abilita la cancellazione differita. read() è un cancellation point POSIX:
     * pthread_cancel() la interrompe immediatamente in modo pulito. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    /* Leggo ciò che ottengo dal CAN finché lo schermo non viene chiuso */
    while (1)
    {
        nBytesRead = read(data->canSoket, &rxMsgFrame, sizeof(struct can_frame));
        if(nBytesRead < 0)
        {
            /* Socket chiuso dalla deinit di canLogic.c: uscita pulita via errno*/
            if(errno == EBADF || errno == EINVAL)
            {
                DEBUG_PRINT("[CAN RX] Socket chiuso, thread RX in uscita\n");
                break;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                DEBUG_PRINT("No message to read...\n");
            }
            else
                ERROR_PRINT("CAN read error\n");  

        }
        else if(nBytesRead == 0)
        {
            DEBUG_PRINT("Not so common\n");
        }
        else if(nBytesRead == sizeof(struct can_frame))
        {
            DEBUG_PRINT("CAN received something!!\n");


            /* 
             * Registra il cleanup handler PRIMA di acquisire il mutex. Se pthread_cancel() arriva dentro la sezione critica,
             * il cleanup handler rilascia il mutex automaticamente evitando deadlock nella deinit.
             */
            pthread_cleanup_push(rx_mutex_cleanup_fn, &data->lock);

            /* Blocca il mutex prima della modifica */
            pthread_mutex_lock(&data->lock);

            if(!can_msg_decoder(&rxMsgFrame, &data->vehicle_struct, &data->canSoket))
            {
                DEBUG_PRINT("CAN understood something!!\n");
            }
            else
                ERROR_PRINT("Invalid CAN ID\n");

            /* Rilascia il mutex alla fine della modifica */
            pthread_mutex_unlock(&data->lock);

            /* Mutex avvenuto quindi deregistro l'handler */
            pthread_cleanup_pop(0);

            nBytesRead = 0;

        }
    }
    return NULL;
}

/* ---------------------------------------------------------------
   INIT
   --------------------------------------------------------------- */

uint8_t can_protocol_init(int *s)
{

    struct sockaddr_can canAddr;
    struct ifreq ifr;
    struct can_filter canFilter[FILTER_SIZE];

    if((*s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0)
    {
        ERROR_PRINT("Can Socket Error");
        return 1;
    }

    strcpy(ifr.ifr_name, "can0");
    ioctl(*s, SIOCGIFINDEX, &ifr);

    canAddr.can_family = AF_CAN;
    canAddr.can_ifindex = ifr.ifr_ifindex;

    if(bind(*s, (struct sockaddr *) &canAddr, sizeof(canAddr)) < 0)
    {
        ERROR_PRINT("Can Bind Error");
        return 1;
    }

    return 0;
}
