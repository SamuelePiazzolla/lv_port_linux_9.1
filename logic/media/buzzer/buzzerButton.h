#ifndef BUZZER_BUTTON_H
#define BUZZER_BUTTON_H

// Percorso del dispositivo di input esposto da gpio-keys-polled
#define INPUT_DEVICE_PATH   "/dev/input/event0"

// Debounce software: tempo minimo tra due pressioni valide (ms), ne esiste già uno hardware di 50 ms
#define BTN_DEBOUNCE_MS     200

int buzzer_buttons_start(void);     // @brief Fa partire il thread per gestire i pulsanti fisici @return -1 Fail | 0 Success
void buzzer_buttons_stop(void);     // @brief Ferma il thread per la gestione dei pulsanti fisici

#endif