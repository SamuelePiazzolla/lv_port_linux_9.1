#ifndef BUZZER_BUTTON_H
#define BUZZER_BUTTON_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

// Chip e linee GPIO (da: gpioinfo)
#define GPIO_CHIP_NAME  "gpiochip0"
#define GPIO_LINE_BTN1  12      // "usr_btn_1" — tone up
#define GPIO_LINE_BTN2  18      // "usr_btn_2" — tone down

// Debounce: tempo minimo tra due pressioni valide sullo stesso tasto (ms)
#define BTN_DEBOUNCE_MS 200

/*
=====================================
    FUNZIONI
=====================================
*/

int buzzer_buttons_start(void);     // @brief  Avvia il thread di ascolto dei pulsanti fisici. @return  0 successo | -1 errore
void buzzer_buttons_stop(void);     // @brief  Ferma il thread di ascolto e libera le risorse GPIO.

#endif // BUZZER_BUTTONS_H