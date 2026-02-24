#include "../../logic.h"
#include "wifiLogic.h"
#include "wpa_ctrl.h"
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>

/* 
=======================================
    STATE MACHINES E DEFINIZIONI
=======================================
*/

/*
 * Macchina a stati per la SCANSIONE.
 */
typedef enum {
    WIFI_SCAN_IDLE,            /* Nessuna scan in volo                              */
    WIFI_SCAN_WAITING_EVENT,   /* SCAN inviata, aspettiamo SCAN-RESULTS             */
    WIFI_SCAN_READING_RESULTS, /* SCAN-RESULTS ricevuto, leggiamo SCAN_RESULTS      */
    WIFI_SCAN_COOLDOWN         /* Cooldown tra una scan automatica e la successiva  */
} WifiScanState;

/*
 * Macchina a stati per l'OPERAZIONE in corso.
 */
typedef enum {
    WIFI_OP_NONE,               /* Nessuna operazione critica in corso  */
    WIFI_OP_CONNECTING,         /* Connessione in corso                 */
    WIFI_OP_DISCONNECTING       /* Disconnessione in corso              */
} WifiOpState;

/* Tipi di evento WiFi riconoscibili in arrivo da wpa_supplicant */
typedef enum {
    WIFI_EVENT_SCAN_RESULTS,        /* CTRL-EVENT-SCAN-RESULTS  */
    WIFI_EVENT_SCAN_FAILED,         /* CTRL-EVENT-SCAN-FAILED   */
    WIFI_EVENT_DISCONNECTED,        /* CTRL-EVENT-DISCONNECTED  */
    WIFI_EVENT_CONNECTED,           /* CTRL-EVENT-CONNECTED     */
    WIFI_EVENT_AUTH_REJECT,         /* CTRL-EVENT-AUTH-REJECT   */
    WIFI_EVENT_ASSOC_REJECT,        /* CTRL-EVENT-ASSOC-REJECT  */
    WIFI_EVENT_NETWORK_NOT_FOUND,   /* CTRL-EVENT-NETWORK-NOT-FOUND */
    WIFI_EVENT_UNKNOWN              /* Tutto il resto           */
} WifiEvent;

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

/* Array condiviso per popolare la lista */
static NetDevice devices[MAX_DEVICES];              /* Array di dispositivi wifi trovati*/
static int device_count = 0;                        /* Numero di dispositivi WiFi trovati*/

/* Connessioni wpa_ctrl persistenti per tutto il ciclo di vita del modulo */
static struct wpa_ctrl *cmd_ctrl = NULL;            /* Canale comandi (sincrono)   */
static struct wpa_ctrl *mon_ctrl = NULL;            /* Canale eventi  (asincrono)  */

/* Timer LVGL per il polling degli eventi wpa_supplicant */
static lv_timer_t *monitor_timer = NULL;

/* network_id assegnato da ADD_NETWORK durante la connessione */
static int pending_network_id = -1;  

/* --- State machine scan --- */
static WifiScanState scan_state = WIFI_SCAN_IDLE;   /* Stato attuale della scan */
static int fail_retry_ticks = 0;                    /* Backoff rimanente dopo SCAN-FAILED                         */
static int cooldown_ticks = 0;                      /* Pausa rimanente tra una scan e la successiva               */
static int waiting_timeout_ticks = 0;               /* Watchdog: tick massimi in WAITING_EVENT                  */

/* --- State machine operazione --- */
static WifiOpState op_state = WIFI_OP_NONE;         /* Stato attuale delle operazioni WiFi*/
static int op_timeout_ticks = 0;                    /* Tick rimanenti prima di dichiarare l'operazione fallita    */

/* 
=======================================
    PROTOTIPI
=======================================
*/

/* -------------------------------------------------------------------------
 * wifi_monitor_cb  (callback timer LVGL)
 *
 * Chiamata ogni WPA_MONITOR_INTERVAL_MS ms nel contesto del tick LVGL.
 * Strutturata in tre blocchi INDIPENDENTI che girano tutti ad ogni tick:
 *
 *  BLOCCO 1 – Drain eventi
 *      Legge TUTTI i messaggi disponibili sul socket mon_ctrl.
 *      Ogni evento viene instradato sia alla scan che all'operazione.
 *      Non fa mai return prematuro: serve sempre drenare il socket.
 *
 *  BLOCCO 2 – Watchdog operazione (connessione / disconnessione)
 *      Decrementa op_timeout_ticks e, se scade, notifica il fallimento.
 *      Gira SEMPRE, indipendentemente dallo stato della scan.
 *
 *  BLOCCO 3 – State machine scan
 *      Gestisce il ciclo IDLE --> WAITING --> READING --> COOLDOWN --> IDLE.
 *      Gira SEMPRE, ma non interferisce con l'operazione in corso.
 * 
 * ------------------------------------------------------------------------- */
static void wifi_monitor_cb(lv_timer_t *t);
static void wifi_parse_scan_results(const char *raw, struct wpa_ctrl *ctrl);                    // Ottiene i valori dei campi dei dispositivi dall'output della scan e aggiorna/inserisce i dispositivi nell'array devices
static void wifi_get_connected_bssid(struct wpa_ctrl *ctrl, char *out, size_t out_size);        // Ottiene il bssid della rete a cui si è connessi se esiste
static SecurityType wifi_parse_security(const char *flags);                                     // Ottiene il tipo di sicurezza della rete
static WifiEvent wifi_classify_event(const char *buf);                                          // Determima il tipo di evento wifi che si è verificato                                          
static void wifi_monitor_start(void);                                                           // Se non attivo, attiva il monitoring facendolo girare sul timer LVGL  
static void wifi_monitor_stop(void);                                                            // Se attivo, ferma il monitoring
static void wifi_remove_pending_network(void);                                                  // Invia il comando REMOVE NETWORK <pending_network_id> e azzera pending_network_id


/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* -------------------------------------------------------------------------
 * CONNESSIONE / DISCONNESSIONE
 * ------------------------------------------------------------------------- */

int wifi_disconnect(void)
{
    if (!cmd_ctrl) return -1;

    if (op_state != WIFI_OP_NONE)
    {
        INFO_PRINT("Operazione critica in corso, non posso effettuare la disconnessione\n");
        ui_log_async("Operazione critica in corso, non posso effettuare la disconnessione, ritenta");
        return -1;
    }

    char reply[16];
    size_t reply_len = sizeof(reply);

    /* Invio richiesta di fisconnessione */
    if (wpa_ctrl_request(cmd_ctrl, "DISCONNECT", 10, reply, &reply_len, NULL) == 0)
    {
        INFO_PRINT("WiFi: Richiesta di disconnessione inviata.\n");

        op_state = WIFI_OP_DISCONNECTING;
        op_timeout_ticks = WPA_DISCONNECTION_TICKS;

        return 0;
    }

    ERROR_PRINT("WiFi: Errore durante l'invio della disconnessione.\n");
    return -1;
}

int wifi_connect_to(NetDevice device, char *password)
{
    if (!cmd_ctrl) return -1;

    if (op_state != WIFI_OP_NONE)
    {
        INFO_PRINT("Operazione critica in corso, non posso effettuare la connessione\n");
        ui_log_async("Operazione critica in corso, non posso effettuare la connessione, ritenta");
        return -1;
    }

    char cmd[256], reply[64];
    size_t reply_len;
    int net_id;

    /* 1. Pulizia pending_network_id rete precedente se rimasta sporca */
    wifi_remove_pending_network();

    /* 2. ADD_NETWORK --> ottieni network_id e mettilo in pending_network_id*/
    reply_len = sizeof(reply) - 1;
    if (wpa_ctrl_request(cmd_ctrl, "ADD_NETWORK", 11, reply, &reply_len, NULL) != 0)
    { 
        ERROR_PRINT("WiFi: ADD_NETWORK fallito\n"); 
        return -1; 
    }
    reply[reply_len] = '\0';
    net_id = atoi(reply);
    if (net_id < 0)
    { 
        ERROR_PRINT("WiFi: ADD_NETWORK risposta non valida: '%s'\n", reply); 
        return -1; 
    }
    pending_network_id = net_id;

    snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", net_id, device.name);
    reply_len = sizeof(reply) - 1;

    /* 3. SET ssid se la risposta OK ad ADD_NETWORK */
    if (wpa_ctrl_request(cmd_ctrl, cmd, strlen(cmd), reply, &reply_len, NULL) != 0 || strncmp(reply, "OK", 2) != 0)
    { 
        ERROR_PRINT("WiFi: SET_NETWORK ssid fallito\n"); 
        wifi_remove_pending_network(); 
        return -1; 
    }

    /* 4. SET credenziali se necessario */
    if (password && password[0] != '\0')
    {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", net_id, password);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", net_id);
    }
    reply_len = sizeof(reply) - 1;
    if (wpa_ctrl_request(cmd_ctrl, cmd, strlen(cmd), reply, &reply_len, NULL) != 0 || strncmp(reply, "OK", 2) != 0)
    { 
        ERROR_PRINT("WiFi: SET_NETWORK credenziali fallito\n"); 
        wifi_remove_pending_network(); 
        return -1; 
    }

    /* 5. SELECT_NETWORK --> si disconnette dalla rete connessa (se presente) e avvia la connessione con la rete indicata */
    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", net_id);
    reply_len = sizeof(reply) - 1;
    if (wpa_ctrl_request(cmd_ctrl, cmd, strlen(cmd), reply, &reply_len, NULL) != 0 || strncmp(reply, "OK", 2) != 0)
    { 
        ERROR_PRINT("WiFi: SELECT_NETWORK fallito\n"); 
        wifi_remove_pending_network(); 
        return -1; 
    }

    /* 6. Arma watchdog e torna al chiamante: l'esito arriva via eventi */
    op_state = WIFI_OP_CONNECTING;
    op_timeout_ticks = WPA_CONNECTION_TICKS;
    INFO_PRINT("WiFi: connessione avviata (net_id=%d, ssid='%s')\n", net_id, device.name);
    return 0;
}

static void wifi_remove_pending_network(void)
{
    if (pending_network_id < 0 || !cmd_ctrl)
        return;
    char cmd[32];
    char reply[16];
    size_t reply_len = sizeof(reply);
    snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", pending_network_id);
    wpa_ctrl_request(cmd_ctrl, cmd, strlen(cmd), reply, &reply_len, NULL);
    pending_network_id = -1;
}

/* -------------------------------------------------------------------------
 * SCANSIONE
 * ------------------------------------------------------------------------- */

void scanWifiNet(void)
{
    INFO_PRINT("WiFi: invio comando di SCAN...\n");

    /* 0. Verifico che sia tutto inizializzato */

    if (!cmd_ctrl || !mon_ctrl)
    {
        ERROR_PRINT("WiFi: non inizializzato, impossibile avviare scan!\n");
        return;
    }

    /* 1. Invio la richiesta di scansione */
    char reply[64];
    size_t reply_len = sizeof(reply) - 1;

    int ret = wpa_ctrl_request(cmd_ctrl, "SCAN", 4, reply, &reply_len, NULL);

    if (ret == -2)
    {
        ERROR_PRINT("WiFi: timeout durante invio SCAN\n");
        return;
    }
    if (ret != 0)
    {
        ERROR_PRINT("WiFi: invio SCAN fallito (ret=%d)\n", ret);
        return;
    }

    /* 2. Leggo se risposta alla richiesta di scansione */
    reply[reply_len] = '\0';
    DEBUG_PRINT("WiFi: risposta a SCAN '%s'\n", reply);

    /* 3. Imposto stato e variabili per gestione scansione */
    scan_state = WIFI_SCAN_WAITING_EVENT;
    fail_retry_ticks = 0;
    waiting_timeout_ticks = WPA_SCAN_WAIT_TIMEOUT_TICKS;

    INFO_PRINT("WiFi: scan avviata, attendo SCAN-RESULTS (watchdog %d tick = %.1f s)...\n",
               WPA_SCAN_WAIT_TIMEOUT_TICKS, WPA_SCAN_WAIT_TIMEOUT_TICKS * WPA_MONITOR_INTERVAL_MS / 1000.0f);
    
    ui_log_async("--- SCANSIONE WIFI AVVIATA ---");
}

/* -------------------------------------------------------------------------
 * MONITOR CALLBACK
 * ------------------------------------------------------------------------- */

static void wifi_monitor_cb(lv_timer_t *t)
{
    (void)t;

    if (!mon_ctrl || !cmd_ctrl)
        return;
       
    /* ======================================================================
     * BLOCCO 1: DRAIN EVENTI
     *
     * Svuota completamente il socket degli eventi ad ogni tick.
     * Ogni evento viene instradato sia alla logica scan che alla logica
     * dell'operazione in corso.
     *
     * ====================================================================== */
    char event_buf[1024];

    /* Giro finche ci sono eventi in attesa */
    while (wpa_ctrl_pending(mon_ctrl) > 0)
    {
        size_t event_len = sizeof(event_buf) - 1;

        /* Leggo l'evento attuale */
        if (wpa_ctrl_recv(mon_ctrl, event_buf, &event_len) != 0)
        {
            ERROR_PRINT("WiFi: errore in wpa_ctrl_recv()\n");
            break;
        }

        event_buf[event_len] = '\0';
        DEBUG_PRINT("WiFi [evento]: %s\n", event_buf);

        /* In base all'evento modifico stato state machines e imposto variabili se necessario */
        switch (wifi_classify_event(event_buf))
        {
            /* --- eventi scansione --- */
            case WIFI_EVENT_SCAN_RESULTS:
                    DEBUG_PRINT("WiFi: SCAN-RESULTS ricevuto\n");
                    /* Accetto l'evento solo lo stavo aspettando.*/
                    if (scan_state == WIFI_SCAN_WAITING_EVENT)
                        scan_state = WIFI_SCAN_READING_RESULTS;
                break;

            case WIFI_EVENT_SCAN_FAILED:
                    ERROR_PRINT("WiFi: SCAN-FAILED --> backoff di %d tick (%.1f sec)\n",
                                WPA_SCAN_FAIL_RETRY_TICKS, WPA_SCAN_FAIL_RETRY_TICKS * WPA_MONITOR_INTERVAL_MS / 1000.0f);
                    /* Modifico la state machine per farle iniziare il backoff dovuto al fail della scan*/
                    if (scan_state == WIFI_SCAN_WAITING_EVENT)
                    {
                        scan_state = WIFI_SCAN_IDLE;
                        fail_retry_ticks = WPA_SCAN_FAIL_RETRY_TICKS;
                    }
                break;

            /* --- eventi operazione --- */
            case WIFI_EVENT_CONNECTED:
                    DEBUG_PRINT("WiFi: evento CONNECTED\n");
                    /* Accetto l'evento solo se lo stavo aspettando */
                    if (op_state == WIFI_OP_CONNECTING)
                    {
                        op_state = WIFI_OP_NONE;
                        op_timeout_ticks = 0;
                        pending_network_id = -1;  /* La rete rimane nel profilo, azzeriamo solo il tracker */
                        lv_async_call(connectionSuccess, NULL);
                        scanWifiNet(); /* Aggiorna la lista per riflettere la connessione */
                    }
                break;

            case WIFI_EVENT_AUTH_REJECT: 
            case WIFI_EVENT_ASSOC_REJECT:
            case WIFI_EVENT_NETWORK_NOT_FOUND:
                    DEBUG_PRINT("WiFi: evento di fallimento connessione\n");
                    if (op_state == WIFI_OP_CONNECTING)
                    {
                        op_state = WIFI_OP_NONE;
                        op_timeout_ticks = 0;
                        wifi_remove_pending_network();
                        lv_async_call(connectionFailed, NULL);
                        scanWifiNet();
                    }
                break;

            case WIFI_EVENT_DISCONNECTED:
                    DEBUG_PRINT("WiFi: evento DISCONNECTED\n");
                    /* Accetto l'evento solo se lo stavo aspettando */
                    if (op_state == WIFI_OP_DISCONNECTING)
                    {
                        DEBUG_PRINT("WiFi: disconnesso dalla rete selezionata");
                        op_state = WIFI_OP_NONE;
                        op_timeout_ticks = 0;
                        lv_async_call(disconnectionSuccess, NULL);
                        scanWifiNet(); /* Aggiorna la lista per riflettere la disconnessione */
                    }
                    else if (op_state == WIFI_OP_CONNECTING)
                    {
                        DEBUG_PRINT("WiFi: disconnessione della rete a cui eri collegato per connettersi ad una nuova avvenuta con successo\n");
                    }
                break;

            case WIFI_EVENT_UNKNOWN: DEBUG_PRINT("Evento sconosciuto\n"); break;
            default: DEBUG_PRINT("Evento sconosciuto\n"); break;
        }
    }

    /* ======================================================================
     * BLOCCO 2: WATCHDOG OPERAZIONE  
     * ====================================================================== */
    if (op_state != WIFI_OP_NONE)
    {
        if (op_timeout_ticks > 0)
        {
            op_timeout_ticks--;

            if (op_timeout_ticks == 0)
            {
                ERROR_PRINT("WiFi: timeout operazione (op_state=%d)\n", op_state);

                /* Salviamo op_state prima di azzerarlo per sapere cosa notificare */
                WifiOpState failed_op = op_state;
                op_state = WIFI_OP_NONE;

                switch (failed_op)
                {
                    case WIFI_OP_DISCONNECTING: lv_async_call(disconnectionFailed, NULL); break;
                    case WIFI_OP_CONNECTING: wifi_remove_pending_network(); lv_async_call(connectionFailed, NULL); break;
                    default:break;
                }
                /* Risincronizziamo la UI con lo stato reale*/
                scanWifiNet();
            }
        }
    }

    /* ======================================================================
     * BLOCCO 3: STATE MACHINE SCAN
     * ====================================================================== */
    switch (scan_state)
    {
        /* -----------------------------------------------------------------
         * IDLE con eventuale backoff dopo SCAN-FAILED
         * ----------------------------------------------------------------- */
        case WIFI_SCAN_IDLE:
                if (fail_retry_ticks > 0)
                {
                    fail_retry_ticks--;
                    DEBUG_PRINT("WiFi: backoff dopo SCAN-FAILED, tick rimanenti: %d\n", fail_retry_ticks);

                    if (fail_retry_ticks == 0)
                    {
                        INFO_PRINT("WiFi: backoff terminato, riprovo scan...\n");
                        scanWifiNet();
                    }
                }
            break;

        /* -----------------------------------------------------------------
         * COOLDOWN: pausa tra le scan per non saturare il driver
         * ----------------------------------------------------------------- */
        case WIFI_SCAN_COOLDOWN:
                cooldown_ticks--;
                DEBUG_PRINT("WiFi: cooldown, tick rimanenti: %d\n", cooldown_ticks);

                if (cooldown_ticks <= 0)
                {
                    INFO_PRINT("WiFi: cooldown terminato, riavvio scan...\n");
                    scanWifiNet();
                }
            break;

        /* -----------------------------------------------------------------
         * WAITING_EVENT: watchdog per evitare di aspettare troppo la scan
         * ----------------------------------------------------------------- */
        case WIFI_SCAN_WAITING_EVENT:
            waiting_timeout_ticks--;
            DEBUG_PRINT("WiFi: attendo SCAN-RESULTS, watchdog tick rimanenti: %d\n", waiting_timeout_ticks);

            if (waiting_timeout_ticks <= 0)
            {
                ERROR_PRINT("WiFi: watchdog scaduto in WAITING_EVENT, avvio backoff di %d tick (%.1f s)\n",
                            WPA_SCAN_FAIL_RETRY_TICKS, WPA_SCAN_FAIL_RETRY_TICKS * WPA_MONITOR_INTERVAL_MS / 1000.0f);
                scan_state = WIFI_SCAN_IDLE;
                fail_retry_ticks = WPA_SCAN_FAIL_RETRY_TICKS;
            }
            break;

        /* -----------------------------------------------------------------
         * READING_RESULTS: legge SCAN_RESULTS e aggiorna la UI
         * ----------------------------------------------------------------- */
        case WIFI_SCAN_READING_RESULTS:
        {
            static char raw[8192];
            size_t raw_len = sizeof(raw) - 1;

            /* Richiedo risultati della scan */
            int ret = wpa_ctrl_request(cmd_ctrl, "SCAN_RESULTS", 12, raw, &raw_len, NULL);
            if (ret == 0)
            {
                raw[raw_len] = '\0';
                wifi_parse_scan_results(raw, cmd_ctrl);
            }
            else
            {
                ERROR_PRINT("WiFi: SCAN_RESULTS fallito (ret=%d)\n", ret);
            }

            scan_state = WIFI_SCAN_COOLDOWN;
            cooldown_ticks = WPA_SCAN_COOLDOWN_TICKS;
            INFO_PRINT("WiFi: cooldown avviato (%d tick = %.1f s)\n",
                       cooldown_ticks, cooldown_ticks * WPA_MONITOR_INTERVAL_MS / 1000.0f);
            break;
        }

        default:break;
    }
}

/* -------------------------------------------------------------------------
 * PARSING RISULTATI SCAN
 * ------------------------------------------------------------------------- */

static void wifi_parse_scan_results(const char *raw, struct wpa_ctrl *ctrl)
{
    /* 0. Pulisco la lista di dispositivi e controllo di avere un risultato utile dalla scan*/
    wifi_clear_devices();

    if (!raw || raw[0] == '\0')
    {
        DEBUG_PRINT("WiFi: SCAN_RESULTS vuoto.\n");
        createDeviceList(devices, device_count);
        return;
    }

    /* 1. Ottengo il dispositivo connesso se esistente */
    char connected_bssid[DEVICE_ADDR_LEN] = {0};
    wifi_get_connected_bssid(ctrl, connected_bssid, sizeof(connected_bssid));
    DEBUG_PRINT("WiFi: BSSID connesso attualmente: '%s'\n", connected_bssid);

    /* 2. Salto la riga di intestazione */
    const char *line = strchr(raw, '\n');
    if (!line)
    {
        DEBUG_PRINT("WiFi: SCAN_RESULTS senza righe di dati.\n");
        createDeviceList(devices, device_count);
        return;
    }
    line++;

    /* 3. Finche non finisco le righe o non riempio completamente l'array, aggiorno l'array di devices */
    while (*line != '\0' && device_count < MAX_DEVICES)
    {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);

        if (line_len > 0)
        {
            char buf[512];
            if (line_len >= sizeof(buf)) line_len = sizeof(buf) - 1;
            memcpy(buf, line, line_len);
            buf[line_len] = '\0';

            char bssid[20], signal[10], ssid[DEVICE_NAME_LEN], flags[128];
            if (sscanf(buf, "%19s\t%*s\t%9s\t%127s\t%127[^\n]", bssid, signal, flags, ssid) >= 4)
            {
                int16_t current_rssi = (int16_t)atoi(signal);
                int found_idx = -1;

                /* Controllo se il dispositivo esiste già nell'array, considerando SSID univoco*/
                for (int i = 0; i < device_count; i++)
                {
                    if (strcmp(devices[i].name, ssid) == 0)
                    {
                        found_idx = i;
                        break;
                    }
                }

                if (found_idx != -1)
                {
                    /* Aggiorno il dispositivo solo se ha una rssi migliore rispetto al suo access point precedente*/
                    if (current_rssi > devices[found_idx].rssi)
                    {
                        NetDevice *d = &devices[found_idx];
                        d->rssi = current_rssi;
                        strncpy(d->address, bssid, DEVICE_ADDR_LEN - 1);
                        d->address[DEVICE_ADDR_LEN - 1] = '\0';
                        d->connected = (connected_bssid[0] != '\0' && strcmp(d->address, connected_bssid) == 0);
                        d->wifi_sec_type = wifi_parse_security(flags);
                    }
                }
                else
                {
                    /* Aggiungo il nuovo dispositivo all'array*/
                    NetDevice *d = &devices[device_count];

                    strncpy(d->name, ssid, DEVICE_NAME_LEN - 1);
                    d->name[DEVICE_NAME_LEN - 1] = '\0';

                    strncpy(d->address, bssid, DEVICE_ADDR_LEN - 1);
                    d->address[DEVICE_ADDR_LEN - 1] = '\0';

                    d->rssi = current_rssi;

                    d->connected = (connected_bssid[0] != '\0' &&
                                    strcmp(d->address, connected_bssid) == 0);

                    d->wifi_sec_type = wifi_parse_security(flags);

                    device_count++;

                    DEBUG_PRINT("WiFi [rete %02d]: SSID='%s'  BSSID=%s  RSSI=%d dBm  Sec=%d  Connected=%d\n",
                                device_count, d->name, d->address, d->rssi, d->wifi_sec_type, d->connected);
                }
            }
        }
        line = end ? end + 1 : line + line_len;
    }

    DEBUG_PRINT("WiFi: Scan completata. Trovati %d SSID univoci.\n", device_count);
    createDeviceList(devices, device_count);
}

static void wifi_get_connected_bssid(struct wpa_ctrl *ctrl, char *bssid_out, size_t len)
{
    if (!ctrl || !bssid_out || len == 0)
        return;

    bssid_out[0] = '\0';

    char buf[2048];
    size_t buf_len = sizeof(buf) - 1;

    /* Richiedo lo stato del mio dispositivo per vedere se è collegato a qualcosa */
    int ret = wpa_ctrl_request(ctrl, "STATUS", 6, buf, &buf_len, NULL);
    if (ret != 0)
    {
        DEBUG_PRINT("WiFi: STATUS fallito (ret=%d), BSSID connesso sconosciuto\n", ret);
        return;
    }
    buf[buf_len] = '\0';

    /* Controllo che il mio dispositivo sia connesso ad una rete */
    if (strstr(buf, "wpa_state=COMPLETED") == NULL)
        return;

    /* Tokenizzo lo stringa di risposta a status, in modo da essere comodo ad ottenere i campi */
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);

    while (line)
    {
        if (strncmp(line, "bssid=", 6) == 0)
        {
            strncpy(bssid_out, line + 6, len - 1);
            bssid_out[len - 1] = '\0';
            return;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static WifiEvent wifi_classify_event(const char *buf)
{
    if (strstr(buf, WPA_EVENT_SCAN_RESULTS)     != NULL) return WIFI_EVENT_SCAN_RESULTS;
    if (strstr(buf, WPA_EVENT_SCAN_FAILED)      != NULL) return WIFI_EVENT_SCAN_FAILED;
    if (strstr(buf, WPA_EVENT_CONNECTED)        != NULL) return WIFI_EVENT_CONNECTED;
    if (strstr(buf, WPA_EVENT_DISCONNECTED)     != NULL) return WIFI_EVENT_DISCONNECTED;
    if (strstr(buf, WPA_EVENT_AUTH_REJECT)      != NULL) return WIFI_EVENT_AUTH_REJECT;
    if (strstr(buf, WPA_EVENT_ASSOC_REJECT)     != NULL) return WIFI_EVENT_ASSOC_REJECT;
    if (strstr(buf, WPA_EVENT_NETWORK_NOT_FOUND)!= NULL) return WIFI_EVENT_NETWORK_NOT_FOUND;
    return WIFI_EVENT_UNKNOWN;
}

static SecurityType wifi_parse_security(const char *flags)
{
    if (!flags) return WIFI_OPEN;

    if (strstr(flags, "WPA3") != NULL || strstr(flags, "SAE") != NULL) return WIFI_WPA3;
    if (strstr(flags, "WPA2") != NULL) return WIFI_WPA2;
    if (strstr(flags, "[WPA-") != NULL) return WIFI_WPA;
    if (strstr(flags, "WEP")  != NULL) return WIFI_WEP;

    return WIFI_OPEN;
}

/* -------------------------------------------------------------------------
 * START / STOP MONITOR
 * ------------------------------------------------------------------------- */

static void wifi_monitor_start(void)
{
    if (monitor_timer != NULL)
    {
        DEBUG_PRINT("WiFi: monitor già attivo.\n");
        return;
    }

    monitor_timer = lv_timer_create(wifi_monitor_cb, WPA_MONITOR_INTERVAL_MS, NULL);
    if (!monitor_timer)
    {
        ERROR_PRINT("WiFi: impossibile creare il timer LVGL per il monitor!\n");
        return;
    }

    INFO_PRINT("WiFi: monitor eventi avviato (polling ogni %d ms)\n", WPA_MONITOR_INTERVAL_MS);
}

static void wifi_monitor_stop(void)
{
    if (monitor_timer == NULL)
    {
        DEBUG_PRINT("WiFi: monitor già fermo.\n");
        return;
    }

    lv_timer_delete(monitor_timer);
    monitor_timer = NULL;
    scan_state = WIFI_SCAN_IDLE;
    op_state = WIFI_OP_NONE;
    fail_retry_ticks = 0;
    cooldown_ticks = 0;
    waiting_timeout_ticks = 0;
    op_timeout_ticks = 0;

    INFO_PRINT("WiFi: monitor eventi fermato.\n");
}

/* -------------------------------------------------------------------------
 * INIT / DEINIT
 * ------------------------------------------------------------------------- */

int logic_init_wifi_mode(void)
{
    INFO_PRINT("--- INIT WIFI MODE ---\n");

    /* 1. Apro canale comandi */
    cmd_ctrl = wpa_ctrl_open(WPA_CTRL_SOCKET_PATH);
    if (!cmd_ctrl)
    {
        ERROR_PRINT("WiFi: impossibile aprire cmd_ctrl su '%s'\n", WPA_CTRL_SOCKET_PATH);
        return -1;
    }

    /* 2. Imposto il timeout usando SO_RCVTIMEO */
    struct timeval rcvtv =
    {
        .tv_sec  = WPA_CMD_RECV_TIMEOUT_SEC,
        .tv_usec = 0
    };
    if (setsockopt(wpa_ctrl_get_fd(cmd_ctrl), SOL_SOCKET, SO_RCVTIMEO, &rcvtv, sizeof(rcvtv)) < 0)
    {
        ERROR_PRINT("WiFi: setsockopt SO_RCVTIMEO fallito (non fatale, timeout sarà 10s)\n");
        return -1;
    }
    else
        DEBUG_PRINT("WiFi: SO_RCVTIMEO impostato a %ds su cmd_ctrl\n", WPA_CMD_RECV_TIMEOUT_SEC);

    INFO_PRINT("WiFi: canale comandi aperto.\n");

    /* 3. Apro il canale di monitoring e lo faccio partire */
    mon_ctrl = wpa_ctrl_open(WPA_CTRL_SOCKET_PATH);
    if (!mon_ctrl)
    {
        ERROR_PRINT("WiFi: impossibile aprire mon_ctrl su '%s'\n", WPA_CTRL_SOCKET_PATH);
        wpa_ctrl_close(cmd_ctrl);
        cmd_ctrl = NULL;
        return -1;
    }

    if (wpa_ctrl_attach(mon_ctrl) != 0)
    {
        ERROR_PRINT("WiFi: wpa_ctrl_attach fallito.\n");
        wpa_ctrl_close(mon_ctrl);
        wpa_ctrl_close(cmd_ctrl);
        mon_ctrl = NULL;
        cmd_ctrl = NULL;
        return -1;
    }
    INFO_PRINT("WiFi: canale eventi aperto e registrato.\n");

    wifi_monitor_start();

    INFO_PRINT("----------------------\n");
    ui_log_async("--- MODULO WIFI INIZIALIZZATO ---");
    return 0;
}

void logic_deinit_wifi_mode(void)
{
    INFO_PRINT("--- DEINIT WIFI MODE ---\n");

    wifi_monitor_stop();
    wifi_clear_devices();

    if (mon_ctrl)
    {
        if (wpa_ctrl_detach(mon_ctrl) != 0)
            ERROR_PRINT("WiFi: detach evento fallito.\n");
        wpa_ctrl_close(mon_ctrl);
        mon_ctrl = NULL;
        INFO_PRINT("WiFi: canale eventi chiuso.\n");
    }

    if (cmd_ctrl)
    {
        wpa_ctrl_close(cmd_ctrl);
        cmd_ctrl = NULL;
        INFO_PRINT("WiFi: canale comandi chiuso.\n");
    }

    INFO_PRINT("------------------------\n");
    ui_log_async("--- MODULO WIFI DEINIZIALIZZATO ---");
}

/* -------------------------------------------------------------------------
 * FUNZIONI PUBBLICHE PER LA GUI
 * ------------------------------------------------------------------------- */

void wifi_clear_devices(void)
{
    device_count = 0;
}

int wifi_get_device_count(void)
{
    return device_count;
}