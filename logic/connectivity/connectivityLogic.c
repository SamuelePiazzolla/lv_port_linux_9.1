#include "../logic.h"
#include "connectivityLogic.h"
#include "nfc/nfcLogic.h"
#include "./bth/bthLogic.h"
#include "./wifi/wifiLogic.h"
#include <stdarg.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/

typedef struct 
{
    NetDevice devices[MAX_DEVICES];
    int count;
} DeviceListMsg;            // Struct per rappresentare il messaggio in arrivo da un'interfaccia con i device trovati e il loro numero 

typedef struct {
    char text[128];
} LogMsg;                   // Struct per rappresentare il messaggio da inviare alla textarea relativo ad una singola operazione (device trovato, tentativo connessione...) 

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static ConnectionMode currentMode = NONE_CONNECTIVITY_MODE;     // Stato attuale del sistema
static NetDevice devSelected;                                   // Device selezionato per connessione
static GDBusMethodInvocation *pending_bt_invocation = NULL;     // Variabile per tenere traccia dell'invocazione in sospeso durante il pairing Bluetooth, in modo da poter rispondere alla richiesta di conferma del codice di accoppiamento
static GMutex pending_invocation_mutex;                         // Mutex per proteggere l'invocazione pendente da accessi concorrenti (es. timeout pairing e risposta utente)
static lv_obj_t * active_pairing_overlay = NULL;                // Overlay per la message box di pairing Bluetooth, tenuto in una var globale per poterlo distruggere da qualsiasi punto del codice quando il pairing finisce o fallisce

/* 
=======================================
    PROTOTIPI FUNZIONI
=======================================
*/

/**
 * 
 *
 * WIFI_MODE  --> disconnessione rete
 * BTH_MODE   --> unpair Bluetooth
 */
static void logic_disconnect_selected(void);   
/**
 * 
 *
 * WIFI_MODE  --> connessione alla rete
 * BTH_MODE   --> pairing Bluetooth
 */
static void logic_connect_selected(void);  
static void ui_create_device_buttons_cb(void *param);               // Funzione di callback che genera i bottoni a partire dalla struct contenente l'array di device e il loro numero
static void device_button_event_cb(lv_event_t *e);                  // Funzione di callback per gestire il click su un bottone generato
static void ui_log_cb(void *param);                                 // Funzione di callback per fare append di un messaggio nella textarea
static void ui_log_clear_cb(void *param);                           // Funzione di callback per la pulizia della textarea
static void setDevSelected(NetDevice* dev);                         // Funzione per impostare il device selezionato tramite il bottone
static void ui_password_clear(void* param);                         // Funzione per pulizia campo password
static void ui_clean_password_zone();                               // Nascondo campo password e tastiera
static void destroyDeviceList();                                    // Funzione per pulire il container dei button dei device
static bool device_requires_password(const NetDevice *dev);         // Funzione per vedere se il dispositivo selezionato ha bisogno o meno della password
static bool wifi_requires_password(const NetDevice *dev);           // Funzione per determinare se il dispositivo wifi ha bisogno o meno della password
static void msgbox_event_cb(lv_event_t *e);                         // Funzione di callback per gestire la risposta dell'utente alla message box di conferma codice di accoppiamento Bluetooth
static void ui_remove_pairing_popup(void * p);                      // Funzione per chiudere la message box di pairing Bluetooth in modo async-safe, chiamata quando il pairing finisce o fallisce

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

// LOGICA STATO SISTEMA

void setConnectionMode(ConnectionMode newMode) 
{
    if(currentMode == newMode)
        return;

    // deinit modalità precedente
    switch(currentMode) 
    {
        case WIFI_MODE:
            logic_deinit_wifi_mode();
            net_clear_devices();
            ui_log_clear();
            ui_clean_password_zone();
            lv_obj_remove_state(ui_wifiBtn, LV_STATE_CHECKED);
            break;
        case BTH_MODE:
            logic_deinit_bth_mode();
            net_clear_devices();
            ui_log_clear();
            ui_clean_password_zone();
            lv_obj_remove_state(ui_bthBtn, LV_STATE_CHECKED);
            break;
        default:
            break;
    }

    currentMode = newMode;

    // init nuova modalità
    switch(currentMode) 
    {
        case WIFI_MODE:
            if (logic_init_wifi_mode() == 0)
            {
                lv_obj_add_state(ui_wifiBtn, LV_STATE_CHECKED);
                lv_obj_remove_state(ui_scanNetButton, LV_STATE_DISABLED);
            }
            else
            {
                ERROR_PRINT("Inizializzazione del wifi fallita\n");
                ui_log_async("ERRORE: INIZIALIZZAZIONE MODULO WIFI FALLITA, RITENTA");
                currentMode = NONE_CONNECTIVITY_MODE;
            }
            
            break;
        case BTH_MODE:
            if ( logic_init_bth_mode() == 0)
            {
                lv_obj_add_state(ui_bthBtn, LV_STATE_CHECKED);
                lv_obj_remove_state(ui_scanNetButton, LV_STATE_DISABLED);
            }
            else
            {
                ERROR_PRINT("Inizializzazione del bth fallita\n");
                ui_log_async("ERRORE: INIZIALIZZAZIONE MODULO BLUETOOTH FALLITA, RITENTA");
                currentMode = NONE_CONNECTIVITY_MODE;
            }
            break;
        default:
            lv_obj_add_state(ui_scanNetButton, LV_STATE_DISABLED);
            break;
    }
}

ConnectionMode getConnectionMode(void) 
{
    return currentMode;
}

// GESTIONE ARRAY DEVICES

void net_clear_devices()
{
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            wifi_clear_devices();
            break;
        case BTH_MODE:
            bth_clear_devices();
            break;
        default: break;
    }

    // Pulisce anche la UI
    destroyDeviceList();
}   

int net_get_device_count()
{
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            return wifi_get_device_count();
        case BTH_MODE:
            return bth_get_device_count();
        default: return 0; 
    }
}      

// LOG NELLA TEXTAREA

void ui_log_async(const char *fmt, ...)
{
    LogMsg *msg = malloc(sizeof(LogMsg));
    if (!msg) return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg->text, sizeof(msg->text), fmt, args);
    va_end(args);

    lv_async_call(ui_log_cb, msg);
}

void ui_log_cb(void *param)
{
    LogMsg *msg = (LogMsg *)param;

    lv_textarea_add_text(ui_logConnectivityArea, msg->text);
    lv_textarea_add_char(ui_logConnectivityArea, '\n');
    lv_textarea_set_cursor_pos(
        ui_logConnectivityArea,
        LV_TEXTAREA_CURSOR_LAST
    );

    free(msg);
}

void ui_log_clear_cb(void *param)
{
    lv_textarea_set_text(ui_logConnectivityArea, "");
}

void ui_log_clear(void)
{
    lv_async_call(ui_log_clear_cb, NULL);
}

// MESSAGE BOX PER CONFERMA CODICE DI ACCOPPIAMENTO BLUETOOTH

void msgbox_event_cb(lv_event_t *e)
{
    /* --- 1. RECUPERIAMO IL BOTTONE CHE È STATO CLICCATO E L'OVERLAY --- */
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    int scelta = (int)lv_event_get_user_data(e);

    /* --- GESTIONE PROTETTA --- */
    // Blocco l'accesso all'invocazione pendente 
    g_mutex_lock(&pending_invocation_mutex);

    /* --- 2. GESTIONE DELLA LOGICA DBUS --- */
    if (pending_bt_invocation) 
    {
        if (scelta == 1) 
        {
            g_dbus_method_invocation_return_value(pending_bt_invocation, NULL);
            ui_log_async("Pairing ACCETTATO.");
            INFO_PRINT("Pairing ACCETTATO.\n");
        } 
        else 
        {
            g_dbus_method_invocation_return_dbus_error(
                pending_bt_invocation,
                "org.bluez.Error.Rejected",             // Nome dell'errore richiesto da BlueZ
                "L'utente ha rifiutato il pairing"      // Messaggio opzionale
            );
            ui_log_async("Pairing RIFIUTATO.");
            INFO_PRINT("Pairing RIFIUTATO.\n");
        }

    }

    g_object_unref(pending_bt_invocation);  // Rilascio il riferimento all'invocazione dopo aver risposto
    pending_bt_invocation = NULL;           // Azzero la variabile dell'invocazione pendente

    // Rilascio del mutex dopo aver gestito l'invocazione pendente
    g_mutex_unlock(&pending_invocation_mutex);

    /* --- 3. PULIZIA UI --- */
    if (active_pairing_overlay != NULL) 
    {
        lv_obj_delete(active_pairing_overlay);
        active_pairing_overlay = NULL;
    }
}

void bt_clear_pending_invocation(const char *dbus_error, const char *message)
{
    // Blocco l'accesso all'invocazione pendente
    g_mutex_lock(&pending_invocation_mutex);

    // Controlla se c'è un'invocazione pendente da rispondere
    if (pending_bt_invocation == NULL)
    {
        g_mutex_unlock(&pending_invocation_mutex);
        return;
    }

    // Se c'è, rispondi con l'errore DBUS indicato (se presente) e azzera la variabile
    if (dbus_error != NULL)
    {
        g_dbus_method_invocation_return_dbus_error(
            pending_bt_invocation,
            dbus_error,
            message ? message : ""
        );
    }

    g_object_unref(pending_bt_invocation); // Rilascio del riferimento all'invocazione dopo aver risposto
    pending_bt_invocation = NULL;

    // Rilascio del mutex dopo aver gestito l'invocazione pendente
    g_mutex_unlock(&pending_invocation_mutex);

    // Pulisco la UI di pairing se è ancora presente, in modo async-safe
    lv_async_call(ui_remove_pairing_popup, NULL);
}

void ui_show_pairing_popup(const char *dev_path, uint32_t passkey, GDBusMethodInvocation *invocation) 
{
    // 1. Gestione Mutex e Invocazione (Invariata)
    g_mutex_lock(&pending_invocation_mutex);
    pending_bt_invocation = invocation; // Salviamo nella variabile globale l'invocazione pendente, in modo da poterci rispondere da qualsiasi punto del codice (es. msgbox_event_cb) quando l'utente prende una decisione, o da funzioni di timeout se l'utente non risponde entro un certo tempo
    g_mutex_unlock(&pending_invocation_mutex);

    // 2. Pulizia di sicurezza
    if (active_pairing_overlay != NULL) {
        lv_obj_delete(active_pairing_overlay);
        active_pairing_overlay = NULL;
        DEBUG_PRINT("GUI: Pulizia overlay precedente.\n"); 
    }

    // 3. CREAZIONE OVERLAY (Sfondo scuro che blocca i click)
    // Usiamo lv_layer_top() per essere sicuri che sia sopra a TUTTO
    active_pairing_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(active_pairing_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(active_pairing_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(active_pairing_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(active_pairing_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(active_pairing_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 4. CREAZIONE PANEL (Il vero e proprio Popup)
    lv_obj_t * panel = lv_obj_create(active_pairing_overlay);
    lv_obj_set_size(panel, 400, 250); // Dimensioni fisse per sicurezza
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 5. TESTO
    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, "RICHIESTA PAIRING");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0); // O quella che usi

    lv_obj_t * info = lv_label_create(panel);
    char buf[128];
    snprintf(buf, sizeof(buf), "Codice: %06u", passkey);
    lv_label_set_text(info, buf);
    lv_obj_set_style_margin_top(info, 20, 0);

    // 6. CONTENITORE BOTTONI (Flex Row)
    lv_obj_t * btn_cont = lv_obj_create(panel);
    lv_obj_set_size(btn_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_cont, 0, 0); // Trasparente
    lv_obj_set_style_border_width(btn_cont, 0, 0);

    // Bottone OK
    lv_obj_t * btn_ok = lv_button_create(btn_cont);
    lv_obj_t * lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "ABBINA");
    lv_obj_add_event_cb(btn_ok, msgbox_event_cb, LV_EVENT_CLICKED, (void*)1); // Usiamo user_data per distinguere

    // Bottone RIFIUTA
    lv_obj_t * btn_no = lv_button_create(btn_cont);
    lv_obj_set_style_bg_color(btn_no, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t * lbl_no = lv_label_create(btn_no);
    lv_label_set_text(lbl_no, "RIFIUTA");
    lv_obj_add_event_cb(btn_no, msgbox_event_cb, LV_EVENT_CLICKED, (void*)0);

    DEBUG_PRINT("GUI: Popup di pairing creato.\n");
}

void ui_show_pairing_popup_wrapper(void * p) 
{
    DEBUG_PRINT("GUI: ui_show_pairing_popup_wrapper chiamato.\n");
    pairing_popup_data_t *data = (pairing_popup_data_t *)p;
    
    // Chiama la tua funzione esistente che crea la msgbox passando però l'invocation che verrà salvata sotto MUTEX
    ui_show_pairing_popup(data->dev_path, data->passkey, data->invocation);
    
    // Puliamo la memoria della struct
    g_free(data->dev_path);
    g_free(data);
}

void ui_remove_pairing_popup(void * p) 
{
    if (active_pairing_overlay != NULL) 
    {
        lv_obj_delete(active_pairing_overlay); // Cancella overlay e figli (msgbox)
        active_pairing_overlay = NULL;
        DEBUG_PRINT("GUI: Popup rimosso forzatamente (Cancel/Timeout).\n");
    } 
    else 
    {
        DEBUG_PRINT("GUI: Nessun popup da rimuovere.\n");
    }
}

// GESTIONE LISTA BOTTONI

void ui_create_device_buttons_cb(void *param)
{
    DeviceListMsg *msg = (DeviceListMsg *)param;

    destroyDeviceList();

    /* Popola la lista di bottoni */
    for (int i = 0; i < msg->count; i++)
    {
        DEBUG_PRINT("Bottone num: %i\n", i);
        lv_obj_t *btn = lv_btn_create(ui_connectivityBtnContainer);
        lv_obj_set_width(btn, lv_pct(80));
        
        // Se il device è connesso/paired, metto il bottone in stato checked e lo coloro di verde
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2DA041), LV_STATE_CHECKED);
        if (msg->devices[i].connected)
            lv_obj_add_state(btn, LV_STATE_CHECKED);

        lv_obj_t *label = lv_label_create(btn);

        // Creo la stringa del label con nome + stato LOCKED/UNLOCKED
        char label_text[DEVICE_NAME_LEN + 16];

        switch(getConnectionMode()) 
        {
            case WIFI_MODE:
                snprintf(label_text, sizeof(label_text), "%s [%s], Speed: %i dBm",
                        msg->devices[i].name,
                        msg->devices[i].connected ? "CONNECTED" :
                        device_requires_password(&msg->devices[i]) ? "LOCKED" : "UNLOCKED",
                        msg->devices[i].rssi);
                //ui_log_async("|->Rete trovata: %s [%s]", msg->devices[i].name, msg->devices[i].connected ? "CONNECTED" : "NOT CONNECTED"); //TESTING
                break;
            case BTH_MODE:
                snprintf(label_text, sizeof(label_text), "%s [%s], Speed: %i dBm",
                        msg->devices[i].name,
                        msg->devices[i].connected ? "PAIRED" : "NOT PAIRED",
                        msg->devices[i].rssi);
                //ui_log_async("|->Dispositivo trovato: %s [%s]", msg->devices[i].name, msg->devices[i].address); //TESTING
                break;
            default: INFO_PRINT("Errore nella creazione del dispositivo\n"); return;
        }

        lv_label_set_text(label, label_text);
        lv_obj_center(label);
        

        // Creazione del bottone
        NetDevice *copy = malloc(sizeof(NetDevice));
        *copy = msg->devices[i];             // copia il device
        lv_obj_set_user_data(btn, copy);     // salvo la copia nel bottone

        lv_obj_add_event_cb(
            btn,
            device_button_event_cb,
            LV_EVENT_CLICKED,
            NULL
        );
    }

    /* Libero la memoria allocata nel thread DBus */
    free(msg);
}

void createDeviceList(NetDevice foundDevices[], int len)
{
    
    DeviceListMsg *msg = malloc(sizeof(DeviceListMsg));
    if (!msg) return;

    msg->count = len;
    memcpy(msg->devices, foundDevices, sizeof(NetDevice) * len);

    // Blocco l'accesso all'invocazione pendente per evitare che venga modificata mentre stiamo creando la lista 
    g_mutex_lock(&pending_invocation_mutex);
    bool is_pairing_active = (pending_bt_invocation != NULL);
    g_mutex_unlock(&pending_invocation_mutex);

    // Se la MessageBox è attiva, saltiamo l'aggiornamento grafico della lista per evitare che la UI sfarfalli dietro il pop-up di pairing
    if (is_pairing_active) 
    {
        DEBUG_PRINT("La MessageBox di pairing è attiva, salto l'aggiornamento della lista per evitare sfarfallio\n");
        free(msg);
        return; 
    }

    DEBUG_PRINT("Aggiorno la lista di bottoni\n");

    /* Esegue la callback nel thread LVGL */
    lv_async_call(ui_create_device_buttons_cb, msg);
}

void device_button_event_cb(lv_event_t *e)
{
    /* Mostro tastiera, campo password e pulsante connect */
    lv_obj_remove_flag(ui_KeyboardConnectivity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_passwordFieldConnectivity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_connectBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_accessConnectivityCtn, LV_OBJ_FLAG_HIDDEN);

    /* Mi salvo bottone premuto e device corrispondente */
    lv_obj_t *btn = lv_event_get_target(e);
    NetDevice *dev = lv_obj_get_user_data(btn);  // prendo il device salvato
    if (!dev) return;

    setDevSelected(dev);

    // Mostra la tastiera e il campo password solo se serve
    if (!device_requires_password(dev)) 
    {
        lv_obj_add_flag(ui_KeyboardConnectivity, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_passwordFieldConnectivity, LV_OBJ_FLAG_HIDDEN);
    }

    // Imposta lo stato iniziale in cui vedere il bottone connect/disconnect
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            if(dev->connected)
            {
                lv_obj_add_state(ui_connectBtn, LV_STATE_CHECKED);
                lv_label_set_text(ui_labelConnectBtn, "DISCONNECT");
                lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            else
            {
                lv_obj_remove_state(ui_connectBtn, LV_STATE_CHECKED);
                lv_label_set_text(ui_labelConnectBtn, "CONNECT");
                lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED);

            }
            break;
        case BTH_MODE:
            if(dev->connected)
            {
                lv_obj_add_state(ui_connectBtn, LV_STATE_CHECKED);
                lv_label_set_text(ui_labelConnectBtn, "UNPAIR");
                lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            else
            {
                lv_obj_remove_state(ui_connectBtn, LV_STATE_CHECKED);
                lv_label_set_text(ui_labelConnectBtn, "PAIR");
                lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            break;
        default: INFO_PRINT("Errore: modalità di connessione sconosciuta\n"); break;
    }
    
    INFO_PRINT("\n--------------DEVICE SELECTED: %s [%s]--------------\n", devSelected.name, devSelected.address);
    ui_log_async("\n---> DEVICE SELEZIONATO: %s [%s]", devSelected.name, devSelected.address);
}

void destroyDeviceList()
{
    // Per ogni bottone nel container vecchio, liberare la copia del device
    uint32_t child_cnt = lv_obj_get_child_cnt(ui_connectivityBtnContainer);
    for(uint32_t i = 0; i < child_cnt; i++) 
    {
        lv_obj_t *child = lv_obj_get_child(ui_connectivityBtnContainer, i);
        if(!child) continue;
        NetDevice *dev = lv_obj_get_user_data(child);
        if(dev) free(dev);
    }

    // Rimuove i bottoni e le label
    lv_obj_clean(ui_connectivityBtnContainer);
}

// FUNZIONI DI LOGICA CONNESSIONE/DISCONNESSIONE

void logic_connect_disconnect_selected()
{
    if (devSelected.address[0] == '\0')
    {
        DEBUG_PRINT("Errore: nessun device selezionato\n");
        return;
    }

    if (devSelected.connected)
        logic_disconnect_selected();
    else
        logic_connect_selected();
}

void logic_connect_selected() 
{
    INFO_PRINT("\n-------------- CONNESSIONE --------------\n");
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
        {
            ui_log_async("\n--- TENTATIVO DI CONNESSIONE WIFI ---");
            char* password = NULL;

            if (device_requires_password(&devSelected)) 
            {
                const char* ui_pass = lv_textarea_get_text(ui_passwordFieldConnectivity);
                password = strdup(ui_pass);
            }

            if (wifi_connect_to(devSelected, password) < 0)
            {
                INFO_PRINT("Errore di connessione\n");
                ui_log_async("Errore di connessione\n");
                connectionFailed(NULL);
            }
            else
            {
                INFO_PRINT("Connessione avviata in background\n");
                lv_label_set_text(ui_labelConnectBtn, "CONNECTING...");
                lv_obj_add_state(ui_connectBtn, LV_STATE_DISABLED);
            }

            if(password)
            {
                free(password);
            }
            break;
        }
            
        case BTH_MODE:
            ui_log_async("\n--- TENTATIVO DI PAIRING ---");
            // Avviamo il pairing. Se ritorna 0 vuol dire che è partito correttamente, ma non è ancora finito. Se ritorna -1 c'è stato un errore grave nell'avvio del pairing
            if(bth_connect_to(devSelected) < 0)
            {
                INFO_PRINT("Errore critico di pairing\n");
                ui_log_async("Errore critico di pairing.");
                connectionFailed(NULL);
            }
            else
            {
                INFO_PRINT("Pairing avviato in background...\n");
                lv_label_set_text(ui_labelConnectBtn, "PAIRING...");
                lv_obj_add_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            
            break;
        default: INFO_PRINT("ERROR: il pulsante non dovrebbe essere visibie e cliccabile\n"); break;
    }

    INFO_PRINT("\n");
}

void logic_disconnect_selected()
{
    INFO_PRINT("\n-------------- DISCONNESSIONE --------------\n");

    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
        {
            ui_log_async("\n--- TENTATIVO DI DISCONNESSIONE WIFI ---");
            int ris = wifi_disconnect();
            if (ris < 0)
            {
                INFO_PRINT("Errore nella disconnessione\n");
                ui_log_async("Errore nella disconnessione\n");
                disconnectionFailed(NULL);
            }
            else if (ris == 1)
            {
                INFO_PRINT("Device already disconnected\n");
                ui_log_async("Device already disconnected");
                disconnectionSuccess(NULL);  
            }
            else
            {
                INFO_PRINT("Disconnessione avviata in background\n");
                lv_label_set_text(ui_labelConnectBtn, "DISCONNECTING...");
                lv_obj_add_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            break;
        }
        case BTH_MODE:
        {
            ui_log_async("\n--- TENTATIVO DI UNPAIRING ---");
            int ris = bth_disconnect(devSelected);
            if(ris < 0)
            {
                // RemoveDevice non è mai partito: c'è stato un errore grave pre-chiamata, manteniamo lo stato del bottone a UNPAIR/CHECKED e mostriamo un messaggio di errore
                INFO_PRINT("Errore critico pre-chiamata in unpairing\n");
                ui_log_async("Device already unpaired.");
                disconnectionFailed(NULL);
            }
            else if (ris == 1)
            {
                // Device already unpaired
                INFO_PRINT("Device already unpaired\n");
                ui_log_async("Device already unpaired.");
                disconnectionSuccess(NULL);                
            }
            else
            {
                // RemoveDevice è stata avviata in background: consideriamo l'operazione in corso 
                INFO_PRINT("Unpairing avviato in background...\n");
                lv_label_set_text(ui_labelConnectBtn, "UNPAIRING...");
                lv_obj_add_state(ui_connectBtn, LV_STATE_DISABLED);
            }
            break;
        }
        default: INFO_PRINT("ERROR: modalità di connessione sconosciuta\n"); break;
    }

    INFO_PRINT("\n");
} 

void connectionSuccess(void* p)
{
    (void) p; // Parametro inutilizzato
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            INFO_PRINT("Connessione riuscita\n");
            ui_log_async("\n---> CONNESSIONE RIUSCITA\n");
            break;
        case BTH_MODE:
            INFO_PRINT("Pairing riuscito\n");
            ui_log_async("\n---> PAIRING RIUSCITO\n");
            break;
        default:INFO_PRINT("Errore: modalità di connessione sconosciuta\n"); break;
    }

    // Pulisco la zona password e pulsante connessione
    ui_clean_password_zone();
}

void connectionFailed(void* p)
{
    (void) p; // Parametro inutilizzato
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            INFO_PRINT("Connessione fallita\n");
            ui_log_async("\n---> CONNESSIONE FALLITA");
            lv_label_set_text(ui_labelConnectBtn, "CONNECT");
            break;
        case BTH_MODE:
            INFO_PRINT("Pairing fallito\n");
            ui_log_async("\n---> PAIRING FALLITO");
            lv_label_set_text(ui_labelConnectBtn, "PAIR");
            break;
        default:INFO_PRINT("Errore: modalità di connessione sconosciuta\n"); break;
    }

    // Riabilito il pulsante di connessione
    lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED); 

}

void disconnectionSuccess(void* p)
{
    (void) p; // Parametro inutilizzato
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            INFO_PRINT("Disconnessione riuscita\n");
            ui_log_async("\n---> DISCONNESSIONE RIUSCITA\n");
            break;
        case BTH_MODE:
            INFO_PRINT("Unpairing riuscito\n");
            ui_log_async("\n---> UNPAIRING RIUSCITO\n");
            break;
        default:INFO_PRINT("Errore: modalità di connessione sconosciuta\n"); break;
    }

    // Pulisco la zona password e pulsante connessione
    ui_clean_password_zone();
}

void disconnectionFailed(void* p)
{
    (void) p; // Parametro inutilizzato
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            INFO_PRINT("Disconnessione fallita\n");
            ui_log_async("\n---> DISCONNESSIONE FALLITA");
            lv_label_set_text(ui_labelConnectBtn, "DISCONNECT");
            break;
        case BTH_MODE:
            INFO_PRINT("Unpairing fallito\n");
            ui_log_async("\n---> UNPAIRING FALLITO");
            lv_label_set_text(ui_labelConnectBtn, "UNPAIR");
            break;
        default:INFO_PRINT("Errore: modalità di connessione sconosciuta\n"); break;
    }

    // Riabilito il pulsante di disconnessione
    lv_obj_remove_state(ui_connectBtn, LV_STATE_DISABLED); 
}

// SCANSIONE

void logic_scan_network()
{
    INFO_PRINT("\n-------------- SCANSIONE RETE --------------\n");
    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            destroyDeviceList(); // Pulisco la lista dei dispositivi trovati in precedenza        
            scanWifiNet();
            break;
        case BTH_MODE:
            destroyDeviceList(); // Pulisco la lista dei dispositivi trovati in precedenza
            scanBthNet();
            break;
        case NONE_CONNECTIVITY_MODE: ERROR_PRINT("ERROR: il pulsante non dovrebbe essere cliccabile\n"); break;
        default: break;
    }
    INFO_PRINT("--------------------------------------------\n");
}

// INIT E DEINIT SCHERMO

void logic_init_connectivity_screen()
{
    INFO_PRINT("\n--------------------------------------------\n");
    INFO_PRINT("--- INIZIALIZZAZIONE CONNECTIVITY SCREEN ---\n");
    // Inizializzo il mutex per proteggere l'invocazione pendente durante il pairing Bluetooth
    g_mutex_init(&pending_invocation_mutex);    
    setConnectionMode(NONE_CONNECTIVITY_MODE);
    INFO_PRINT("--------------------------------------------\n");
}

void logic_deinit_connectivity_screen()
{
    INFO_PRINT("\n----------------------------------------------\n");
    INFO_PRINT("--- DEINIZIALIZZAZIONE CONNECTIVITY SCREEN ---\n");
    setConnectionMode(NONE_CONNECTIVITY_MODE);
    g_mutex_clear(&pending_invocation_mutex); // Distrugge il mutex
    INFO_PRINT("----------------------------------------------\n");
}

// UTILITIES

void setDevSelected(NetDevice* dev)
{
    strncpy(devSelected.name, dev->name, DEVICE_NAME_LEN-1);
    devSelected.name[DEVICE_NAME_LEN-1] = '\0';
    strncpy(devSelected.address, dev->address, DEVICE_ADDR_LEN-1);
    devSelected.address[DEVICE_ADDR_LEN-1] = '\0';
    devSelected.connected = dev->connected;
    devSelected.rssi = dev->rssi;
    devSelected.wifi_sec_type = dev->wifi_sec_type;
}

void checkRemoveDevice(char* address)
{
    if(address == NULL) return;
    if(strcmp(address, devSelected.address) == 0)
    {
        DEBUG_PRINT("Il device selezionato non è più presente, pulisco la zona password e nascondo tastiera e campo password\n");
        ui_log_async("---> IL DEVICE SELEZIONATO NON ESISTE PIU");
        
        // Se ci sono operazioni in corso sul device selezionato, consideriamole fallite
        if (devSelected.connected)
        {
            disconnectionFailed(NULL);
        }
        else
        {
            connectionFailed(NULL);
        }

        ui_clean_password_zone();
    }
}

bool device_requires_password(const NetDevice *dev)
{
    if (!dev) return false;

    switch(getConnectionMode()) 
    {
        case WIFI_MODE:
            // Se la rete è aperta non serve password altrimenti 
            return wifi_requires_password(dev);
        case BTH_MODE:
            return false;
        default:
            return false;
    }
}

bool wifi_requires_password(const NetDevice *dev)
{
    return dev && dev->wifi_sec_type != WIFI_OPEN && !dev->connected;
}

void ui_password_clear(void *param)
{
    lv_textarea_set_text(ui_passwordFieldConnectivity, "");
}

void ui_clean_password_zone()
{
    /* Pulisco campo password e nascondo tastiera e campo password */
    lv_obj_add_flag(ui_KeyboardConnectivity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_accessConnectivityCtn, LV_OBJ_FLAG_HIDDEN);
    lv_async_call(ui_password_clear, NULL);

    /* Rimuovo il dispositivo scelto */
    memset(&devSelected, 0, sizeof(devSelected));

    DEBUG_PRINT("Campo password, tastiera e dispositivo selezionato puliti\n");
}
