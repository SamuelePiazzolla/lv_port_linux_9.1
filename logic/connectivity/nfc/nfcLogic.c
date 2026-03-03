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
    NFC_STATE_IDLE,           /* In attesa di rilevare un tag, LED spento */
    NFC_STATE_DISPLAY_ACTIVE  /* Tag rilevato, LED acceso per il tempo di visualizzazione */
} nfc_state_t;



/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static char gNfcController_generation = 0;              /* Generazione controller NFC */
static pthread_t nfc_thread;                            /* Thread di polling NFC */
static atomic_bool nfc_running = false;                 /* Flag atomico per controllare l'esecuzione del thread */
static int nfc_handle = -1;                             /* Handle per la comunicazione con il controller NFC */
static nfc_state_t current_state = NFC_STATE_IDLE;      /* Stato corrente del sistema NFC */

/* 
=======================================
    PROTOTIPI 
=======================================
*/

static int nfc_reset_controller(int handle);                // @brief Reset e inizializzazione del controller NFC
static int nfc_start_discovery(int handle);                 // @brief Avvia il discovery loop NFC
static int nfc_restart_discovery(int handle);               // @brief Riavvia il discovery loop dopo rilevamento tag
static int nfc_wait_for_tag(int handle);                    // @brief Attende il rilevamento di un tag NFC @return 0 se tag rilevato, -1 in caso di timeout/errore
static void* nfc_polling_thread(void* arg);                 // @brief Thread di polling NFC, gestisce il ciclo di rilevamento tag con macchina a stati
static void nfc_update_ui_detected(void* user_data);        // @brief Callback per aggiornare l'UI quando un tag viene rilevato
static void nfc_update_ui_cleanup(void* user_data);         // @brief Callback per resettare l'UI
static void nfc_get_timestamp(char* buffer, size_t size);   // @brief Ottiene il timestamp corrente formattato

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* 
---------------------------------------
    UTILITY
---------------------------------------
*/

static void nfc_get_timestamp(char* buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "[%H:%M:%S]", tm_info);
}

/* 
---------------------------------------
    DISCOVERY
---------------------------------------
*/

static int nfc_wait_for_tag(int handle)
{
    char Answer[256];
    int ret;
    
    /* Legge il prossimo messaggio dalla coda del controller NFC */
    /* NOTA: tml_receive() ha un timeout interno di quindi questa funzione BLOCCA per circa 200ms se non ci sono messaggi */
    ret = tml_receive(handle, Answer, sizeof(Answer));
    
    /* Verifica se è una notifica di discovery (RF_INTF_ACTIVATED o RF_DISCOVER) */
    if (ret > 0 && Answer[0] == 0x61 && (Answer[1] == 0x05 || Answer[1] == 0x03)) 
    {
        DEBUG_PRINT("Notifica NFC ricevuta: tipo=0x%02X\n", Answer[1]);
        return 0; /* Tag rilevato */
    }
    
    return -1; /* Nessun tag rilevato o errore */
}

static int nfc_start_discovery(int handle)
{
    char NCIStartDiscovery[] = {0x21, 0x03, 0x09, 0x04, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x06, 0x01};
    char Answer[256];

    tml_transceive(handle, NCIStartDiscovery, sizeof(NCIStartDiscovery), Answer, sizeof(Answer));
    if((Answer[0] != 0x41) || (Answer[1] != 0x03) || (Answer[3] != 0x00)) 
    {
        ERROR_PRINT("Impossibile avviare discovery loop\n");
        return -1;
    }

    INFO_PRINT("Discovery loop avviato con successo\n");
    return 0;
}

static int nfc_restart_discovery(int handle)
{
    char NCIRestartDiscovery[] = {0x21, 0x06, 0x01, 0x03};
    char Answer[256];

    tml_transceive(handle, NCIRestartDiscovery, sizeof(NCIRestartDiscovery), Answer, sizeof(Answer));
    return 0;
}

/* 
---------------------------------------
    UPDATE UI
---------------------------------------
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
    snprintf(message, sizeof(message), "---%s DISPOSITIVO RILEVATO ---\n", timestamp);
    lv_textarea_add_text(ui_nfcTextArea, message);
    
    INFO_PRINT("Tag NFC rilevato - UI aggiornata\n");
}

static void nfc_update_ui_cleanup(void* user_data)
{
    (void)user_data; /* Parametro non utilizzato */
    
    /* Spegni LED */
    lv_obj_clear_state(ui_nfcLed, LV_STATE_CHECKED);
    
    /* Svuota textArea */
    lv_textarea_set_text(ui_nfcTextArea, "");
    
    DEBUG_PRINT("UI NFC pulita\n");
}

/* 
---------------------------------------
    THREAD
---------------------------------------
*/

static void* nfc_polling_thread(void* arg)
{
    (void)arg; /* Parametro non utilizzato */
    
    DEBUG_PRINT("Thread polling NFC avviato\n");
    
    /* Disabilita modalità standby */
    char NCIDisableStandby[] = {0x2F, 0x00, 0x01, 0x00};
    char Answer[256];
    tml_transceive(nfc_handle, NCIDisableStandby, sizeof(NCIDisableStandby), Answer, sizeof(Answer));
    
    /* Avvia discovery iniziale */
    if (nfc_start_discovery(nfc_handle) != 0) 
    {
        ERROR_PRINT("Errore avvio discovery - thread terminato\n");
        atomic_store(&nfc_running, false);
        return NULL;
    }
    
    current_state = NFC_STATE_IDLE;
    struct timespec display_start;
    
    /* Loop principale di polling */
    while (atomic_load(&nfc_running)) 
    {
        switch (current_state) 
        {
            case NFC_STATE_IDLE:
                /* Attesa rilevamento tag */
                DEBUG_PRINT("Stato IDLE - in attesa di tag NFC...\n");
                
                if (nfc_wait_for_tag(nfc_handle) == 0) 
                {
                    INFO_PRINT("Tag NFC rilevato - cambio stato a DISPLAY_ACTIVE\n");
                    current_state = NFC_STATE_DISPLAY_ACTIVE;
                    
                    /* Salva il timestamp di inizio visualizzazione */
                    clock_gettime(CLOCK_MONOTONIC, &display_start);
                    
                    /* Aggiorna UI tramite async call (thread-safe) */
                    lv_async_call(nfc_update_ui_detected, NULL);
                }
            break;
                
            case NFC_STATE_DISPLAY_ACTIVE:
                /* LED acceso, attende che scadano i NFC_DISPLAY_TIME_MS */
                {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    
                    /* Calcola tempo trascorso dall'inizio della visualizzazione */
                    long elapsed_ms = (now.tv_sec - display_start.tv_sec) * 1000 + (now.tv_nsec - display_start.tv_nsec) / 1000000;
                    
                    if (elapsed_ms >= NFC_DISPLAY_TIME_MS) 
                    {
                        INFO_PRINT("Tempo di visualizzazione terminato (%ld ms) - ritorno a IDLE\n", elapsed_ms);
                        current_state = NFC_STATE_IDLE;
                        
                        /* Spegni LED e pulisci UI tramite async call */
                        lv_async_call(nfc_update_ui_cleanup, NULL);
                        
                        /* Riavvia discovery per nuovo ciclo */
                        nfc_restart_discovery(nfc_handle);
                    }
                    else 
                    {
                        /* Controlliamo se è arrivato un messaggio importante */
                        char temp_answer[256];
                        int ret = tml_receive(nfc_handle, temp_answer, sizeof(temp_answer));
                        
                        if (ret > 0) 
                        {
                            
                            /* RF_DEACTIVATE_NTF (0x61 0x06): tag rimosso, aspetto comunque tutti e 3 i secondi*/
                            if (temp_answer[0] == 0x61 && temp_answer[1] == 0x06) 
                            {
                                INFO_PRINT("Tag rimosso durante DISPLAY_ACTIVE - continuo visualizzazione\n");
                            }
                            /* RF_INTF_ACTIVATED (0x61 0x05) o RF_DISCOVER (0x61 0x03): nuovo tag, lo ignoriamo */
                            else if (temp_answer[0] == 0x61 && (temp_answer[1] == 0x05 || temp_answer[1] == 0x03)) 
                            {
                                INFO_PRINT("Nuovo tag rilevato durante DISPLAY_ACTIVE - ignorato (LED già acceso)\n");
                            }
                            /* Altre notifiche: ignorate ma logate */
                            else 
                            {
                                INFO_PRINT("Notifica NFC 0x%02X 0x%02X ricevuta durante DISPLAY_ACTIVE\n", temp_answer[0], temp_answer[1]);
                            }
                            
                        }
                        else 
                        {
                            /* Nessun messaggio, sleep breve */
                            usleep(10 * 1000); // 10ms
                        }
                    }
                    
                }
            break;
        }
    }
    
    DEBUG_PRINT("Thread polling NFC terminato\n");
    return NULL;
}

/* 
---------------------------------------
    INIT / DEINIT
---------------------------------------
*/

static int nfc_reset_controller(int handle)
{
    char NCICoreReset[] = {0x20, 0x00, 0x01, 0x01};
    char NCICoreInit1_0[] = {0x20, 0x01, 0x00};
    char NCICoreInit2_0[] = {0x20, 0x01, 0x02, 0x00, 0x00};
    char Answer[256];
    int NbBytes = 0;
    
    /* Primo reset: il controller potrebbe ancora avere notifiche in coda dal bootstrap */
    tml_transceive(handle, NCICoreReset, sizeof(NCICoreReset), Answer, sizeof(Answer));

    /* Pulizia coda: svuota eventuali notifiche residue dal reset hardware */
    usleep(100 * 1000);
    NbBytes = tml_receive(handle, Answer, sizeof(Answer));

    /* Secondo reset: ora la comunicazione è pulita e la risposta sarà affidabile */
    tml_transceive(handle, NCICoreReset, sizeof(NCICoreReset), Answer, sizeof(Answer));

    /* Pulizia coda finale: svuota eventuali altre notifiche prima di inizializzare */
    usleep(100 * 1000);
    NbBytes = tml_receive(handle, Answer, sizeof(Answer));

    if((NbBytes == 12) && (Answer[0] == 0x60) && (Answer[1] == 0x00) && (Answer[3] == 0x02))
    {
        NbBytes = tml_transceive(handle, NCICoreInit2_0, sizeof(NCICoreInit2_0), Answer, sizeof(Answer));
        if((NbBytes < 19) || (Answer[0] != 0x40) || (Answer[1] != 0x01) || (Answer[3] != 0x00)) 
        {
            ERROR_PRINT("Errore comunicazione con controller NFC\n");
            return -1;
        }
        gNfcController_generation = 3;
    }
    else
    {
        NbBytes = tml_transceive(handle, NCICoreInit1_0, sizeof(NCICoreInit1_0), Answer, sizeof(Answer));
        if((NbBytes < 19) || (Answer[0] != 0x40) || (Answer[1] != 0x01) || (Answer[3] != 0x00)) 
        {
            ERROR_PRINT("Errore comunicazione con controller PN71xx NFC\n");
            return -1;
        }

        /* Identifica generazione controller NXP-NCI */
        int fw_idx = 17 + (uint8_t)Answer[8];
        if (fw_idx >= NbBytes || fw_idx >= (int)sizeof(Answer))
        {
            ERROR_PRINT("Risposta NFC malformata: fw_idx=%d fuori bounds (NbBytes=%d)\n", fw_idx, NbBytes);
            return -1;
        }

        if (Answer[fw_idx] == 0x08)
        {
            gNfcController_generation = 1;
        }
        else if (Answer[fw_idx] == 0x10)
        {
            gNfcController_generation = 2;
        }
        else 
        {
            ERROR_PRINT("Generazione controller NFC non riconosciuta\n");
            return -1;
        }
    }

    /* Log generazione controller rilevata */
    switch(gNfcController_generation) 
    {
        case 1: DEBUG_PRINT("Controller PN7120 rilevato\n"); break;
        case 2: DEBUG_PRINT("Controller PN7150 rilevato\n"); break;
        case 3: DEBUG_PRINT("Controller PN7160 rilevato\n"); break;
        default: ERROR_PRINT("Controller NFC errato rilevato\n"); break;
    }

    return 0;
}

int logic_init_nfc(void)
{
    /* Apri connessione con controller NFC */
    if (tml_open(&nfc_handle) != 0) 
    {
        ERROR_PRINT("Impossibile connettersi al controller NFC\n");
        return -1;
    }
    
    /* Reset e inizializza controller */
    if (nfc_reset_controller(nfc_handle) != 0) 
    {
        ERROR_PRINT("Errore inizializzazione controller NFC\n");
        tml_close(nfc_handle);
        nfc_handle = -1;
        return -1;
    }
    
    /* Avvia thread di polling */
    atomic_store(&nfc_running, true);
    if (pthread_create(&nfc_thread, NULL, nfc_polling_thread, NULL) != 0) 
    {
        ERROR_PRINT("Errore creazione thread polling NFC\n");
        atomic_store(&nfc_running, false);
        tml_close(nfc_handle);
        nfc_handle = -1;
        return -1;
    }

    INFO_PRINT("--------------------------------\n");
    INFO_PRINT("--- NFC SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("--------------------------------\n");
    
    return 0;
}

void logic_deinit_nfc(void)
{
    /* Ferma il thread di polling */
    if (atomic_load(&nfc_running)) 
    {
        DEBUG_PRINT("Arresto thread polling NFC...\n");
        atomic_store(&nfc_running, false);
        pthread_join(nfc_thread, NULL);
        DEBUG_PRINT("Thread polling NFC arrestato\n");
    }
    
    /* Chiudi connessione NFC */
    if (nfc_handle >= 0) 
    {
        tml_close(nfc_handle);
        nfc_handle = -1;
        DEBUG_PRINT("Connessione NFC chiusa\n");
    }
    
    /* Pulisci UI eseguita direttamente, siamo in un evento generato da LVGL */
    nfc_update_ui_cleanup(NULL);

    INFO_PRINT("----------------------------------\n");
    INFO_PRINT("--- NFC SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("----------------------------------\n");
}