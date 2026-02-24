#ifndef CAN_H
#define CAN_H

/* ---------------------------------------------------------------
   	Definizioni
   --------------------------------------------------------------- */

#define MSG_PRIORITY_MASK               0x03
#define MSG_ID_MASK                     0xFF
#define MSG_TYPE_MASK                   0x07
#define NODE_ADDR_MASK                  0xFF

#define MSG_PRIORITY_SHIFT              27
#define MSG_ID_SHIFT                    19
#define MSG_TYPE_SHIFT                  16
#define DEST_NODE_SHIFT                 8

#define DEST_NODE_NUM                   2
#define SRC_NODE_NUM                    2
#define DATA_MSG_TYPE_NUM               2
#define DATA_MSG_ID_NUM                 5
#define MSG_PRIORITY_NUM                4

#define FILTER_SIZE                     (DEST_NODE_NUM * SRC_NODE_NUM * DATA_MSG_TYPE_NUM * DATA_MSG_ID_NUM * MSG_PRIORITY_NUM)

enum canMessage_s{
	DSB_icons = 0x44,
	VMS_data_1_msg = 0x2F2,
	VMS_data_2_msg,
	DSB_data_1_msg = 0x321,
};

enum idMessage_s{
	VMS_data_1_id = 0x00,
	VMS_data_2_id,
	DSB_data_1_id,
};


/* ---------------------------------------------------------------
   Funzioni pubbliche
   --------------------------------------------------------------- */

uint8_t can_protocol_init(int *s);		// Apre il socket per la comunicazione via CAN in lettura
void * can_rx_handler (void * arg);		// In base al numero di dati in arrivo effettua un'operazione oppure un'altra

#endif