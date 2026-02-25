#ifndef CONNECTIVITY_LOGIC_H
#define CONNECTIVITY_LOGIC_H

/*
=====================================
    DEFINIZIONI
=====================================
*/

#define DEVICE_NAME_LEN 128
#define DEVICE_ADDR_LEN 18
#define MAX_DEVICES 128

/*
=====================================
    LIBRERIE
=====================================
*/

#include <glib.h>
#include <gio/gio.h>
#include "nfc/nfcLogic.h"
#include "./bth/bthLogic.h"
#include "./wifi/wifiLogic.h"

/*
=====================================
    STRUTTURE
=====================================
*/

// Struttura per rappresentare la modalità di connessione attuale (Wi-Fi, Bluetooth o nessuna)
typedef enum {
    NONE_CONNECTIVITY_MODE,
    WIFI_MODE,
    BTH_MODE
} ConnectionMode;

// Tipologia di sicurezza WiFi 
typedef enum {
    WIFI_OPEN = 0,
    WIFI_WEP,
    WIFI_WPA,
    WIFI_WPA2,
    WIFI_WPA3
} SecurityType;           

// Struttura per rappresentare un dispositivo trovato (Bluetooth o Wi-Fi)
typedef struct {
    char name[DEVICE_NAME_LEN];     // Nome device 
    char address[DEVICE_ADDR_LEN];  // Indirizzo device

    bool connected;                 /* Se il device è connesso ---|N.B| In bth con connected intendiamo paired*/
    int16_t rssi;                   // Velocità della connessione

    /* --- Wi-Fi only --- */
    SecurityType wifi_sec_type;     // Tipologia di connessine WiFi

} NetDevice;

// Struttura per passare i dati necessari alla MessageBox di pairing come user_data al callback del click sui bottoni della MessageBox
typedef struct {
    char *dev_path;
    uint32_t passkey;
    GDBusMethodInvocation *invocation;
} pairing_popup_data_t;  

/*
=====================================
    FUNZIONI
=====================================
*/

void logic_init_connectivity_screen(void);                                                              // Inizializza lo schermo connectivity
void logic_deinit_connectivity_screen(void);                                                            // Deinizializza lo schermo connectivity                        
void logic_scan_network(void);                                                                          // Logica di scansione della rete a ricerca di dispositivi o connessioni wifi
void setConnectionMode(ConnectionMode mode);                                                            // Funzione per impostare lo stato del sistema attuale
ConnectionMode getConnectionMode(void);                                                                 // Funzione per ottenere lo stato del sistema attuale
void net_clear_devices();                                                                               // Pulisce l'array con i dispositivi trovati
int net_get_device_count();                                                                             // Restituisce il numero di device trovati
void createDeviceList(NetDevice foundDevices[], int len);                                               // Crea la lista di bottoni basandosi sull'array di device trovati chiamando la funzione di callback
void ui_log_async(const char *fmt, ...);                                                                // Chiama la funzione di callback per appendere il messaggio passato nella textarea in modo asincrono
void ui_log_clear(void);                                                                                // Chiama la funzione di callback per pulire la textarea in modo asincrono
void ui_show_pairing_popup_wrapper(void * p);                                                           // Funzione wrapper per chiamare la UI di pairing popup in modo async-safe, riceve una struct con i dati necessari al pairing (dev_path, passkey, invocation)
void bt_clear_pending_invocation(const char *dbus_error, const char *message);                          // Azzera pending_bt_invocation rispondendo all'invocation con l'errore indicato (se presente)
void logic_connect_disconnect_selected(void);                                                           // Logica comune per connect/disconnect, che si occupa di abilitare/disabilitare la logica del bottone connect 
void connectionSuccess(void* p);                                                                        // Funzione da chiamare quando riceviamo conferma di connessione/pairing riuscito, per aggiornare la UI di conseguenza
void connectionFailed(void* p);                                                                         // Funzione da chiamare quando riceviamo conferma di connessione/pairing fallito, per aggiornare la UI di conseguenza   
void disconnectionSuccess(void* p);                                                                     // Funzione da chiamare quando riceviamo conferma di disconnessione/unpairing riuscito, per aggiornare la UI di conseguenza
void disconnectionFailed(void* p);                                                                      // Funzione da chiamare quando riceviamo conferma di disconnessione/unpairing riuscito/fallito, per aggiornare la UI di conseguenza
void checkRemoveDevice(char* address);                                                                  // Funzione che verifica se il dispositivo rimosso è quello selezionato e nel caso aggiorna la UI, se si stavano compiendo operazioni su quel dispositivo le considera fallite e aggiorna la UI di conseguenza

#endif /* CONNECTIVITY_LOGIC_H */