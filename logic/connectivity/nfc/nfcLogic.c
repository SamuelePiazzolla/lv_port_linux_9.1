#define _POSIX_C_SOURCE 199309L

#include "../../logic.h"
#include "nfcLogic.h"
#include "tml.h"
#include "lvgl/lvgl.h"
#include "ui/ui.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/

/* Stati del polling NFC */
typedef enum {
    NFC_STATE_IDLE,           /* In attesa di rilevare un tag */
    NFC_STATE_TAG_PRESENT,    /* Tag rilevato, LED acceso */
    NFC_STATE_COOLDOWN        /* Tag rimosso, attesa prima di nuovo discovery */
} nfc_state_t;

/* Generazione controller NFC */
static char gNfcController_generation = 0;

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

/* Thread di polling NFC */
static pthread_t nfc_thread;

/* Flag atomico per controllare l'esecuzione del thread */
static atomic_bool nfc_running = false;

/* Handle per la comunicazione con il controller NFC */
static int nfc_handle = -1;

/* Stato corrente del sistema NFC */
static nfc_state_t current_state = NFC_STATE_IDLE;

/* 
=======================================
    PROTOTIPI 
=======================================
*/

static int nfc_reset_controller(int handle);
static int nfc_start_discovery(int handle);
static int nfc_restart_discovery(int handle);
static bool nfc_check_tag_present(int handle);
static void* nfc_polling_thread(void* arg);
static void nfc_update_ui_detected(void* user_data);
static void nfc_update_ui_cleanup(void* user_data);
static void nfc_get_timestamp(char* buffer, size_t size);

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/**
 * @brief Ottiene il timestamp corrente formattato
 */
static void nfc_get_timestamp(char* buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "[%H:%M:%S]", tm_info);
}

/**
 * @brief Reset e inizializzazione del controller NFC
 * @note Adattato da reset_controller() in lts00656_dut_sw.c
 */
static int nfc_reset_controller(int handle)
{
    char NCICoreReset[] = {0x20, 0x00, 0x01, 0x01};
    char NCICoreInit1_0[] = {0x20, 0x01, 0x00};
    char NCICoreInit2_0[] = {0x20, 0x01, 0x02, 0x00, 0x00};
    char Answer[256];
    int NbBytes = 0;

    tml_reset(handle);
    tml_transceive(handle, NCICoreReset, sizeof(NCICoreReset), Answer, sizeof(Answer));

    /* Cattura potenziale notifica */
    usleep(100 * 1000);
    NbBytes = tml_receive(handle, Answer, sizeof(Answer));

    tml_transceive(handle, NCICoreReset, sizeof(NCICoreReset), Answer, sizeof(Answer));

    /* Cattura potenziale notifica */
    usleep(100 * 1000);
    NbBytes = tml_receive(handle, Answer, sizeof(Answer));

    if((NbBytes == 12) && (Answer[0] == 0x60) && (Answer[1] == 0x00) && (Answer[3] == 0x02))
    {
        NbBytes = tml_transceive(handle, NCICoreInit2_0, sizeof(NCICoreInit2_0), Answer, sizeof(Answer));
        if((NbBytes < 19) || (Answer[0] != 0x40) || (Answer[1] != 0x01) || (Answer[3] != 0x00)) {
            ERROR_PRINT("Errore comunicazione con controller NFC\n");
            return -1;
        }
        gNfcController_generation = 3;
    }
    else
    {
        NbBytes = tml_transceive(handle, NCICoreInit1_0, sizeof(NCICoreInit1_0), Answer, sizeof(Answer));
        if((NbBytes < 19) || (Answer[0] != 0x40) || (Answer[1] != 0x01) || (Answer[3] != 0x00)) {
            ERROR_PRINT("Errore comunicazione con controller PN71xx NFC\n");
            return -1;
        }

        /* Identifica generazione controller NXP-NCI */
        if (Answer[17 + Answer[8]] == 0x08) {
            gNfcController_generation = 1;
        }
        else if (Answer[17 + Answer[8]] == 0x10) {
            gNfcController_generation = 2;
        }
        else {
            ERROR_PRINT("Generazione controller NFC non riconosciuta\n");
            return -1;
        }
    }

    /* Log generazione controller rilevata */
    switch(gNfcController_generation) {
        case 1: INFO_PRINT("Controller PN7120 rilevato\n"); break;
        case 2: INFO_PRINT("Controller PN7150 rilevato\n"); break;
        case 3: INFO_PRINT("Controller PN7160 rilevato\n"); break;
        default: ERROR_PRINT("Controller NFC errato rilevato\n"); break;
    }

    return 0;
}

/**
 * @brief Avvia il discovery loop NFC
 */
static int nfc_start_discovery(int handle)
{
    char NCIStartDiscovery[] = {0x21, 0x03, 0x09, 0x04, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x06, 0x01};
    char Answer[256];

    tml_transceive(handle, NCIStartDiscovery, sizeof(NCIStartDiscovery), Answer, sizeof(Answer));
    if((Answer[0] != 0x41) || (Answer[1] != 0x03) || (Answer[3] != 0x00)) {
        ERROR_PRINT("Impossibile avviare discovery loop\n");
        return -1;
    }

    INFO_PRINT("Discovery loop avviato con successo\n");
    return 0;
}

/**
 * @brief Riavvia il discovery loop dopo rilevamento tag
 */
static int nfc_restart_discovery(int handle)
{
    char NCIRestartDiscovery[] = {0x21, 0x06, 0x01, 0x03};
    char Answer[256];

    tml_transceive(handle, NCIRestartDiscovery, sizeof(NCIRestartDiscovery), Answer, sizeof(Answer));
    return 0;
}

/**
 * @brief Verifica se un tag NFC è presente tramite polling
 * @return true se tag presente, false altrimenti
 */
static bool nfc_check_tag_present(int handle)
{
    char Answer[256];
    int ret = tml_receive(handle, Answer, sizeof(Answer));
    
    /* Verifica se è una notifica di discovery (RF_INTF_ACTIVATED o RF_DISCOVER) */
    if (ret > 0 && Answer[0] == 0x61 && (Answer[1] == 0x05 || Answer[1] == 0x03)) {
        return true;
    }
    
    return false;
}

/**
 * @brief Callback per aggiornare l'UI quando un tag viene rilevato
 * @note Questa funzione viene chiamata tramite lv_async_call dal thread NFC
 */
static void nfc_update_ui_detected(void* user_data)
{
    (void)user_data; /* Parametro non utilizzato */
    
    char timestamp[32];
    char message[128];
    
    /* Ottieni timestamp corrente */
    nfc_get_timestamp(timestamp, sizeof(timestamp));
    
    /* Accendi LED */
    lv_obj_add_state(ui_nfcLed, LV_STATE_CHECKED);
    
    /* Aggiungi messaggio con timestamp alla textArea */
    snprintf(message, sizeof(message), "%s --- DISPOSITIVO RILEVATO ---\n", timestamp);
    lv_textarea_add_text(ui_nfcTextArea, message);
    
    INFO_PRINT("Tag NFC rilevato - UI aggiornata\n");
}

/**
 * @brief Callback per resettare l'UI in fase di deinit
 * @note Questa funzione viene chiamata tramite lv_async_call
 */
static void nfc_update_ui_cleanup(void* user_data)
{
    (void)user_data; /* Parametro non utilizzato */
    
    /* Spegni LED */
    lv_obj_clear_state(ui_nfcLed, LV_STATE_CHECKED);
    
    /* Svuota textArea */
    lv_textarea_set_text(ui_nfcTextArea, "");
    
    INFO_PRINT("UI NFC pulita\n");
}

/**
 * @brief Thread di polling NFC
 * @details Gestisce il ciclo di rilevamento tag con macchina a stati
 */
static void* nfc_polling_thread(void* arg)
{
    (void)arg; /* Parametro non utilizzato */
    
    INFO_PRINT("Thread polling NFC avviato\n");
    
    /* Disabilita modalità standby */
    char NCIDisableStandby[] = {0x2F, 0x00, 0x01, 0x00};
    char Answer[256];
    tml_transceive(nfc_handle, NCIDisableStandby, sizeof(NCIDisableStandby), Answer, sizeof(Answer));
    
    /* Avvia discovery iniziale */
    if (nfc_start_discovery(nfc_handle) != 0) {
        ERROR_PRINT("Errore avvio discovery - thread terminato\n");
        atomic_store(&nfc_running, false);
        return NULL;
    }
    
    current_state = NFC_STATE_IDLE;
    struct timespec cooldown_start;
    
    /* Loop principale di polling */
    while (atomic_load(&nfc_running)) {
        switch (current_state) {
            case NFC_STATE_IDLE:
                /* Attesa rilevamento tag */
                if (nfc_check_tag_present(nfc_handle)) {
                    INFO_PRINT("Tag rilevato - cambio stato a TAG_PRESENT\n");
                    current_state = NFC_STATE_TAG_PRESENT;
                    
                    /* Aggiorna UI tramite async call (thread-safe) */
                    lv_async_call(nfc_update_ui_detected, NULL);
                    
                    /* Restart discovery per continuare monitoring */
                    nfc_restart_discovery(nfc_handle);
                }
                usleep(50 * 1000); /* 50ms tra controlli */
                break;
                
            case NFC_STATE_TAG_PRESENT:
                /* Monitora se tag è ancora presente */
                if (!nfc_check_tag_present(nfc_handle)) {
                    INFO_PRINT("Tag rimosso - inizio cooldown\n");
                    current_state = NFC_STATE_COOLDOWN;
                    clock_gettime(CLOCK_MONOTONIC, &cooldown_start);
                }
                usleep(50 * 1000); /* 50ms tra controlli */
                break;
                
            case NFC_STATE_COOLDOWN:
                /* Attesa cooldown dopo rimozione tag */
                {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    
                    long elapsed_ms = (now.tv_sec - cooldown_start.tv_sec) * 1000 +
                                     (now.tv_nsec - cooldown_start.tv_nsec) / 1000000;
                    
                    if (elapsed_ms >= NFC_COOLDOWN_TIME_MS) {
                        INFO_PRINT("Cooldown terminato - ritorno a IDLE\n");
                        current_state = NFC_STATE_IDLE;
                        
                        /* Spegni LED tramite async call */
                        lv_async_call(nfc_update_ui_cleanup, NULL);
                        
                        /* Riavvia discovery per nuovo ciclo */
                        nfc_restart_discovery(nfc_handle);
                    }
                    usleep(50 * 1000); /* 50ms tra controlli */
                }
                break;
        }
    }
    
    INFO_PRINT("Thread polling NFC terminato\n");
    return NULL;
}

int logic_init_nfc(void)
{
    INFO_PRINT("--------------------------------\n");
    INFO_PRINT("--- NFC SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("--------------------------------\n");
    
    /* Apri connessione con controller NFC */
    if (tml_open(&nfc_handle) != 0) {
        ERROR_PRINT("Impossibile connettersi al controller NFC\n");
        return -1;
    }
    
    /* Reset e inizializza controller */
    if (nfc_reset_controller(nfc_handle) != 0) {
        ERROR_PRINT("Errore inizializzazione controller NFC\n");
        tml_close(nfc_handle);
        nfc_handle = -1;
        return -1;
    }
    
    /* Avvia thread di polling */
    atomic_store(&nfc_running, true);
    if (pthread_create(&nfc_thread, NULL, nfc_polling_thread, NULL) != 0) {
        ERROR_PRINT("Errore creazione thread polling NFC\n");
        atomic_store(&nfc_running, false);
        tml_close(nfc_handle);
        nfc_handle = -1;
        return -1;
    }
    
    INFO_PRINT("Modulo NFC inizializzato con successo\n");
    return 0;
}

void logic_deinit_nfc(void)
{
    INFO_PRINT("----------------------------------\n");
    INFO_PRINT("--- NFC SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("----------------------------------\n");
    
    /* Ferma il thread di polling */
    if (atomic_load(&nfc_running)) {
        INFO_PRINT("Arresto thread polling NFC...\n");
        atomic_store(&nfc_running, false);
        pthread_join(nfc_thread, NULL);
        INFO_PRINT("Thread polling NFC arrestato\n");
    }
    
    /* Chiudi connessione NFC */
    if (nfc_handle >= 0) {
        tml_reset(nfc_handle);
        tml_close(nfc_handle);
        nfc_handle = -1;
        INFO_PRINT("Connessione NFC chiusa\n");
    }
    
    /* Pulisci UI tramite async call (sempre, in qualsiasi caso) */
    lv_async_call(nfc_update_ui_cleanup, NULL);
    
    INFO_PRINT("Modulo NFC deinizializzato con successo\n");
}