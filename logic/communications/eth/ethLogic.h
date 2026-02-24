#ifndef ETH_LOGIC_H
#define ETH_LOGIC_H

/*
=====================================
    FUNZIONI
=====================================
*/

int  logic_init_eth_mode(void);     // Inizializza la logica per le comunicazioni eth e fa partire il thread eth
void logic_deinit_eth_mode(void);   // Deinizializza la logica per le comunicazioni eth e ferma il thread eth
void sendTestMessageEth(void);      // Richiede l'invio di un messaggio di test eth: iperf3

#endif