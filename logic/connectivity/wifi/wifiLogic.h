#ifndef WIFI_LOGIC_H
#define WIFI_LOGIC_H

#include "../connectivityLogic.h"

/*
=====================================
    DEFINIZIONI
=====================================
*/

#define WPA_CTRL_SOCKET_PATH        "/var/run/wpa_supplicant/wlan0"
#define WPA_MONITOR_INTERVAL_MS     500  /* Intervallo in ms con cui il timer LVGL chiama il monitor degli eventi       */
#define WPA_CMD_RECV_TIMEOUT_SEC    1    /* Timeout SO_RCVTIMEO su cmd_ctrl: protegge il tick LVGL da blocchi lunghi    */
#define WPA_SCAN_FAIL_RETRY_TICKS   10   /* Tick di backoff dopo SCAN-FAILED      (10 × 500ms = 5s)                     */
#define WPA_SCAN_COOLDOWN_TICKS     20   /* Tick di pausa tra una scan e l'altra  (20 × 500ms = 10s)                    */
#define WPA_SCAN_WAIT_TIMEOUT_TICKS 30   /* Tick max in WAITING_EVENT             (30 × 500ms = 15s)                    */
#define WPA_DISCONNECTION_TICKS     8    /* Tick max per attendere DISCONNECTED   ( 8 × 500ms = 4s)                     */
#define WPA_CONNECTION_TICKS        40   /* Tick max per attendere CONNECTED      (40 × 500ms = 20s)                    */

/*
=====================================
    FUNZIONI
=====================================
*/

int logic_init_wifi_mode(void);                          /* (-1 Fail | 0 Success ) Inizializza la logica per le connessioni WiFi              */
void logic_deinit_wifi_mode(void);                       /* Deinizializza la logica per le connessioni WiFi            */
void scanWifiNet(void);                                  /* Scansiona la rete alla ricerca di connessioni WiFi         */
void wifi_clear_devices(void);                           /* Pulisce l'array device wifi                                */
int  wifi_get_device_count(void);                        /* Restituisce il numero di device wifi trovati               */
int  wifi_connect_to(NetDevice device, char *password);  /* -1 Fail | 0 Success: connessione alla rete selezionata     */
int  wifi_disconnect(void);                              /* -1 Fail | 0 Success: disconnessione dalla rete corrente    */

#endif /* WIFI_LOGIC_H */