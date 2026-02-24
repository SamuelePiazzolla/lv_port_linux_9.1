#include "../../logic.h"
#include "buzzerLogic.h"
#include <gpiod.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static BuzzerState buzzState = OFF;   // Stato attuale del buzzer

/* 
=======================================
    PROTOTIPI
=======================================
*/

int turnOffBuzzer(void);        // (-1 Fail | 0 Success) Accende fisicamente il buzzer    
int turnOnBuzzer(void);         // (-1 Fail | 0 Success) Spegne fisicamente il buzzer

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* 
---------------------------------------
    TONE CHANGE
---------------------------------------
*/

/* 
---------------------------------------
    ON / OFF
---------------------------------------
*/

void logic_btn_buzzer_click_handler(void)
{
    switch (buzzState)
    {
        case OFF:
            DEBUG_PRINT("Accendo il buzzer\n");
            if (turnOffBuzzer != 0)
            {
                DEBUG_PRINT("Errore nell'accesione del buzzer");
            }
            lv_label_set_text(ui_buzzerBtnLabel, "SPEGNI IL BUZZER");
            lv_obj_add_state(ui_buzzerBtn, LV_STATE_CHECKED);
        break;
        case ON:
            DEBUG_PRINT("Spengo il buzzer\n");
            if(turnOffBuzzer != 0)
            {
                ERROR_PRINT("Errore nello spegnimento del buzzer\n");
            }
            lv_label_set_text(ui_buzzerBtnLabel, "ACCENDI IL BUZZER");
            lv_obj_remove_state(ui_buzzerBtn, LV_STATE_CHECKED);
        break;
        default:/*Non dovrebbe mai succedere*/break;
    }
}

int turnOffBuzzer()
{
    // Eseguo la system call per spegnere
    buzzState = OFF;
    DEBUG_PRINT("Buzzer spento\n");
    return 0;
}

int turnOnBuzzer()
{
    // Eseguo la system call per accendere
    buzzState = ON;
    DEBUG_PRINT("Buzzer acceso\n");
    return 0;
}

/* 
---------------------------------------
    INIT / DEINIT
---------------------------------------
*/

void logic_init_buzzer_screen(void)
{
    INFO_PRINT("-----------------------------------\n");
    // Inizializzo il buffer con le system call e avvio il thread che aspetta la pressione dei pulsanti fisici

    INFO_PRINT("--- BUZZER SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("-----------------------------------\n");
}

void logic_deinit_buzzer_screen(void)
{
    INFO_PRINT("-------------------------------------\n");
    // Pulisco eventuali variabili globali e la memoria, fermo il thread di ascolto dei pulsanti

    INFO_PRINT("--- BUZZER SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("-------------------------------------\n");

}

