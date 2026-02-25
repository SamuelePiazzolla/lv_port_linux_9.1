#ifndef NFC_LOGIC_H
#define NFC_LOGIC_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

/* Tempo di cooldown dopo rimozione tag (in millisecondi) */
#define NFC_COOLDOWN_TIME_MS 1500

/*
=====================================
    FUNZIONI
=====================================
*/

int logic_init_nfc(void);       // @brief Inizializza il modulo NFC e avvia il thread di polling @return -1 Fail | 0 Success
void logic_deinit_nfc(void);    // @brief Deinizializza il modulo NFC e termina il thread di polling

#endif /* NFC_LOGIC_H */