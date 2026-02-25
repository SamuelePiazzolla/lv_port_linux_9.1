#ifndef BUZZER_PWM_H
#define BUZZER_PWM_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

// Percorsi sysfs del canale PWM
#define PWM_CHIP_PATH       "/sys/class/pwm/pwmchip0"
#define PWM_CHANNEL         "5"
#define PWM_CHANNEL_PATH    PWM_CHIP_PATH "/pwm" PWM_CHANNEL

// Parametri PWM di default (tono 800 Hz, duty cycle 50%)
#define PWM_DEFAULT_PERIOD_NS   1250000UL   // 800 Hz  → period = 1/f in nanosecondi
#define PWM_MIN_PERIOD_NS        500000UL   // 2000 Hz (tono massimo)
#define PWM_MAX_PERIOD_NS       4000000UL   // 250 Hz  (tono minimo)
#define PWM_TONE_STEP_NS         125000UL   // Step di variazione del tono per ogni pressione

/*
=====================================
    FUNZIONI
=====================================
*/

int buzzer_pwm_setup(void);             // @brief  Esporta il canale PWM e imposta period e duty_cycle di default. @return  0 successo | -1 errore
int buzzer_pwm_enable(void);            // @brief  Abilita l'uscita PWM (accende il buzzer). @return  0 successo | -1 errore
int buzzer_pwm_disable(void);           // @brief  Disabilita l'uscita PWM (spegne il buzzer). @return  0 successo | -1 errore
int buzzer_pwm_tone_up(void);           // @brief  Aumenta il tono (diminuisce il period) di un passo. Non supera PWM_MIN_PERIOD_NS @return  0 successo | -1 errore | 1 già al massimo
int buzzer_pwm_tone_down(void);         // @brief  Abbassa il tono (aumenta il period) di un passo. Non supera PWM_MAX_PERIOD_NS. @return  0 successo | -1 errore | 1 già al minimo
uint32_t buzzer_pwm_get_period(void);   // @brief  Restituisce il period attuale in nanosecondi.

#endif // BUZZER_PWM_H