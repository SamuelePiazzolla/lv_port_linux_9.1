#ifndef BUZZER_LOGIC_H
#define BUZZER_LOGIC_H
/*
=====================================
    DEFINIZIONI
=====================================
*/

typedef enum {
    OFF,
    ON
} BuzzerState;

/*
=====================================
    FUNZIONI
=====================================
*/

void logic_init_buzzer_screen(void);        // @brief Init dello schermo del buzzer
void logic_deinit_buzzer_screen(void);      // @brief Deinit dello schermo del buzzer
void logic_btn_buzzer_click_handler(void);  // @brief Gestisce il click in base allo stato del buzzer
int buzzer_logic_is_on(void);               // @brief Restituisce lo stato attuale del buzzer @return 1 buzzer ON | 0 buzzer OFF

#endif