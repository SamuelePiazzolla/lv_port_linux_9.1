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

void logic_init_buzzer_screen(void);        // Init dello schermo del buzzer
void logic_deinit_buzzer_screen(void);      // Deinit dello schermo del buzzer
void logic_btn_buzzer_click_handler(void);  // Gestisce il click in base allo stato del buzzer


#endif