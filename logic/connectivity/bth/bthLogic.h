#ifndef BTH_LOGIC_H
#define BTH_LOGIC_H

#include "../connectivityLogic.h"

/*
=====================================
    FUNZIONI
=====================================
*/

int logic_init_bth_mode(void);                  // ( -1 Fail | 0 Success ) Inizializza la logica per le connessioni bluetooth
void logic_deinit_bth_mode(void);               // Deinizializza la logica per le connessioni bluetooth
void scanBthNet(void);                          // Scansiona la rete alla ricerca di dispositivi bluetooth
void bth_clear_devices();                       // Pulisce l'array device bth 
int bth_get_device_count();                     // Restituisce il numero di device bth trovati
int bth_connect_to(NetDevice device);           // (-1 Fail | 0 Success) Tenta di connettersi al dispositivo selezionato
int bth_disconnect(NetDevice device);           // (-1 Fail | 0 Success | 1 Already Unpair) Disconnette dal dispositivo selezionato (se esisteva una connesisone)

#endif