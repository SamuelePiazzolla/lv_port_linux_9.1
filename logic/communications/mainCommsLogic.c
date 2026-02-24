#include "../logic.h"
#include "mainCommsLogic.h"
#include "./eth/ethLogic.h"
#include "./rs/rsLogic.h"

/* 
=======================================
    DEFINIZIONI
=======================================
*/

typedef struct {
    char text[LOG_MSG_SIZE];    
} LogMsg;

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static CommunicationMode currentMode = NONE_COMMUNICATION_MODE;    // Stato attuale del sistema

/* 
=======================================
    PROTOTIPI FUNZIONI
=======================================
*/

static void ui_comms_log_cb(void *param);        // Funzione di callback per fare append di un messaggio nella textarea
static void ui_comms_log_clear_cb(void *param);  // Funzione di callback per la pulizia della textarea

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* -------------------------------------------------------------------------
 * LOG NELLA TEXTAREA
 * ------------------------------------------------------------------------- */

void ui_comms_log_async(const char *fmt, ...)
{
    LogMsg *msg = malloc(sizeof(LogMsg));
    if (!msg) return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg->text, sizeof(msg->text), fmt, args);
    va_end(args);

    lv_async_call(ui_comms_log_cb, msg);
}

static void ui_comms_log_cb(void *param)
{
    LogMsg *msg = (LogMsg *)param;

    lv_textarea_add_text(ui_commsTextArea, msg->text);
    lv_textarea_add_char(ui_commsTextArea, '\n');
    lv_textarea_set_cursor_pos(ui_commsTextArea, LV_TEXTAREA_CURSOR_LAST);

    free(msg);
}

static void ui_comms_log_clear_cb(void *param)
{
    (void)param; 
    lv_textarea_set_text(ui_commsTextArea, "");
}

void ui_comms_log_clear(void)
{
    lv_async_call(ui_comms_log_clear_cb, NULL);
}

/* -------------------------------------------------------------------------
 * LOGICA STATO SISTEMA
 * ------------------------------------------------------------------------- */

void setCommunicationMode(CommunicationMode newMode)
{
    if (currentMode == newMode)
        return;

    // ---- Deinit modalità precedente ----
    switch (currentMode)
    {
        case ETH_MODE:
            logic_deinit_eth_mode();
            ui_comms_log_clear();
            lv_obj_remove_state(ui_ethBtn, LV_STATE_CHECKED);
            break;
        case RS_MODE:
            logic_deinit_rs_mode();
            ui_comms_log_clear();
            lv_obj_remove_state(ui_rsBtn, LV_STATE_CHECKED);
            break;
        default:
            break;
    }

    currentMode = newMode;

    // ---- Init nuova modalità ----
    switch (currentMode)
    {
        case ETH_MODE:
            if (logic_init_eth_mode() == 0)
            {
                lv_obj_add_state(ui_ethBtn, LV_STATE_CHECKED);
                lv_obj_remove_state(ui_testCommsBtn, LV_STATE_DISABLED);
            }
            else
            {
                ERROR_PRINT("Inizializzazione eth fallita\n");
                ui_comms_log_async("INIZIALIZZAZIONE ETH FALLITA, RITENTA");
                currentMode = NONE_COMMUNICATION_MODE;  /* rollback */
                lv_obj_add_state(ui_testCommsBtn, LV_STATE_DISABLED);
            }
            break;

        case RS_MODE:
            if (logic_init_rs_mode() == 0)
            {
                lv_obj_add_state(ui_rsBtn, LV_STATE_CHECKED);
                lv_obj_remove_state(ui_testCommsBtn, LV_STATE_DISABLED);
            }
            else
            {
                ERROR_PRINT("Inizializzazione rs-485 fallita\n");
                ui_comms_log_async("INIZIALIZZAZIONE RS-485 FALLITA, RITENTA");
                currentMode = NONE_COMMUNICATION_MODE;  /* rollback */
                lv_obj_add_state(ui_testCommsBtn, LV_STATE_DISABLED);
            }
            break;

        default:
            lv_obj_add_state(ui_testCommsBtn, LV_STATE_DISABLED);
            break;
    }
}

CommunicationMode getCommunicationMode(void)
{
    return currentMode;
}

/* -------------------------------------------------------------------------
 * PULSANTI
 * ------------------------------------------------------------------------- */

void logic_send_test_message_comms(void)
{
    switch (currentMode)
    {
        case ETH_MODE:
            sendTestMessageEth();
            break;
        case RS_MODE:
            sendTestMessageRs();
            break;
        default:
            INFO_PRINT("ERROR: non dovresti poter premere questo pulsante\n");
            break;
    }
}

/* -------------------------------------------------------------------------
 * INIT E DEINIT SCHERMO
 * ------------------------------------------------------------------------- */

void logic_init_communication_screen(void)
{
    INFO_PRINT("\n---------------------------------------------\n");
    INFO_PRINT("--- INIZIALIZZAZIONE COMMUNICATION SCREEN ---\n");

    setCommunicationMode(NONE_COMMUNICATION_MODE);

    INFO_PRINT("---------------------------------------------\n");
}

void logic_deinit_communication_screen(void)
{
    INFO_PRINT("\n-----------------------------------------------\n");
    INFO_PRINT("--- DEINIZIALIZZAZIONE COMMUNICATION SCREEN ---\n");

    setCommunicationMode(NONE_COMMUNICATION_MODE);

    INFO_PRINT("-----------------------------------------------\n");
}