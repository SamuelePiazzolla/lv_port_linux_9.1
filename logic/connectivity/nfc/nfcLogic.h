#ifndef NFC_LOGIC_H
#define NFC_LOGIC_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

/* Tempo di visualizzazione LED dopo rilevamento tag (in millisecondi) */
#define NFC_DISPLAY_TIME_MS 3000  /* 3 secondi */

/*
=====================================
    FUNZIONI
=====================================
*/

int logic_init_nfc(void);       // @brief Inizializza il modulo NFC e avvia il thread di polling @return -1 Fail | 0 Success
void logic_deinit_nfc(void);    // @brief Deinizializza il modulo NFC e termina il thread di polling

#endif /* NFC_LOGIC_H */