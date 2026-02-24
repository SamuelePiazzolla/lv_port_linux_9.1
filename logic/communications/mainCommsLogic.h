#ifndef MAIN_COMMS_LOGIC_H
#define MAIN_COMMS_LOGIC_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

#define LOG_MSG_SIZE 256    // Dimensione massima testo loggabile nella textArea

typedef enum {
    NONE_COMMUNICATION_MODE,
    ETH_MODE,
    RS_MODE
} CommunicationMode;

/*
=====================================
    FUNZIONI
=====================================
*/

void setCommunicationMode(CommunicationMode mode);      // Funzione per impostare lo stato del sistema attuale
CommunicationMode getCommunicationMode(void);           // Funzione per ottenere lo stato del sistema attuale
void logic_init_communication_screen(void);             // Inizializza lo schermo communication
void logic_deinit_communication_screen(void);           // Deinizializza lo schermo communication
void logic_send_test_message_comms(void);               // Invia un messaggio di test sul mezzo di comunicazione desiderato
void ui_comms_log_async(const char *fmt, ...);          // Chiama la funzione di callback per appendere il messaggio passato nella textarea in modo asincrono
void ui_comms_log_clear(void);                                // Chiama la funzione di callback per pulire la textarea in modo asincrono

#endif