#include "../../logic.h"
#include "buzzerLogic.h"
#include "buzzerButton.h"

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static BuzzerState buzzState = OFF;     // Variabile che conserva lo stato del buffer   

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/
/* 
---------------------------------------
    ON / OFF (pulsante UI)
---------------------------------------
*/

void logic_btn_buzzer_click_handler(void)
{
    switch (buzzState)
    {
        case OFF:
            DEBUG_PRINT("Accendo il buzzer\n");
            if (buzzer_pwm_enable() != 0)                               
            {                                                           
                ERROR_PRINT("Errore nell'accensione del buzzer\n");
                return;
            }
            INFO_PRINT("--- BUZZER ACCESO ---\n");
            buzzState = ON;
            lv_obj_add_state(ui_buzzerBtn, LV_STATE_CHECKED);
        break;

        case ON:
            DEBUG_PRINT("Spengo il buzzer\n");
            if (buzzer_pwm_disable() != 0)
            {
                ERROR_PRINT("Errore nello spegnimento del buzzer\n");
                return;
            }
            INFO_PRINT("--- BUZZER SPENTO ---\n");
            buzzState = OFF;
            lv_obj_remove_state(ui_buzzerBtn, LV_STATE_CHECKED);
        break;

        default: /* Non dovrebbe mai succedere */ break;
    }
}

int buzzer_logic_is_on(void)
{
    return (buzzState == ON) ? 1 : 0;
}

/* 
---------------------------------------
    INIT / DEINIT
---------------------------------------
*/
void logic_init_buzzer_screen(void)
{
    INFO_PRINT("-----------------------------------\n");

    // Setup buzzer
    if (buzzer_pwm_setup() != 0)
    {
        ERROR_PRINT("Errore durante il setup del buzzer PWM\n");
    }

    // Avvio thread per pulsanti fisici
    if (buzzer_buttons_start() != 0)
    {
        ERROR_PRINT("Errore durante l'avvio del thread bottoni\n");
    }

    INFO_PRINT("--- BUZZER SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("-----------------------------------\n");   
}


void logic_deinit_buzzer_screen(void)
{
    INFO_PRINT("-------------------------------------\n");

    // Prima fermiamo il thread
    buzzer_buttons_stop();

    // Spegnimento di sicurezza prima di uscire
    if (buzzState == ON)
    {
        buzzer_pwm_disable();
        buzzState = OFF;
    }

    // Pulizia bottoni
    lv_obj_remove_state(ui_buzzerBtn, LV_STATE_CHECKED);

    INFO_PRINT("--- BUZZER SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("-------------------------------------\n");
}