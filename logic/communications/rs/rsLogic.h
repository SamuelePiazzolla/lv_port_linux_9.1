#ifndef RS_LOGIC_H
#define RS_LOGIC_H

/*
=====================================
    FUNZIONI
=====================================
*/

int logic_init_rs_mode(void);     // (-1 Fail | 0 Success) Inizializza la logica per le comunicazioni RS-485
void logic_deinit_rs_mode(void);  // Deinizializza la logica RS-485 e termina il processo slave se attivo
void sendTestMessageRs(void);     // Avvia (o riavvia) il processo slave RS-485 e ne logga l'output

#endif