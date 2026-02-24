#ifndef CAN_LOGIC_H
#define CAN_LOGIC_H

/*
=====================================
    FUNZIONI PUBBLICHE
=====================================
*/

/**
 * @brief  Inizializza il modulo CAN e fa partire i thread di ricezione e aggiornamento schermo
 */
void  logic_init_can_mode(void);

/**
 * @brief  Deinizializza il modulo CAN: chiude i thread e pulisce le variabili
 */
void logic_deinit_can_mode(void);

#endif /* CAN_LOGIC_H */