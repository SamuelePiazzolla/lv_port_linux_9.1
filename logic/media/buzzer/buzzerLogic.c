#include "../../logic.h"
#include "buzzerLogic.h"
#include "buzzerPwm.h"

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static BuzzerState buzzState = OFF;     

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
            lv_label_set_text(ui_buzzerBtnLabel, "SPEGNI IL BUZZER");
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
            lv_label_set_text(ui_buzzerBtnLabel, "ACCENDI IL BUZZER");
            lv_obj_remove_state(ui_buzzerBtn, LV_STATE_CHECKED);
        break;

        default: /* Non dovrebbe mai succedere */ break;
    }
}

/* 
---------------------------------------
    INIT / DEINIT
---------------------------------------
*/
void logic_init_buzzer_screen(void)
{
    INFO_PRINT("-----------------------------------\n");

    if (buzzer_pwm_setup() != 0)
    {
        ERROR_PRINT("Errore durante il setup del buzzer PWM\n");
    }

    INFO_PRINT("--- BUZZER SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("-----------------------------------\n");   
}


void logic_deinit_buzzer_screen(void)
{
    INFO_PRINT("-------------------------------------\n");

    // Spegnimento di sicurezza prima di uscire
    if (buzzState == ON)
    {
        buzzer_pwm_disable();
        buzzState = OFF;
    }

    // Pulizia bottoni
    lv_label_set_text(ui_buzzerBtnLabel, "ACCENDI IL BUZZER");
    lv_obj_remove_state(ui_buzzerBtn, LV_STATE_CHECKED);

    INFO_PRINT("--- BUZZER SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("-------------------------------------\n");
}