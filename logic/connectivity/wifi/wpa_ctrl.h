/*
 * wpa_supplicant/hostapd control interface library
 * Standalone Header - Linux Embedded / LVGL Integration
 *
 * Derivato da: wpa_supplicant/src/common/wpa_ctrl.h
 * Copyright (c) 2004-2017, Jouni Malinen <j@w1.fi>
 * BSD License
 *
 * Ridotto rispetto all'originale:
 *  - Rimossi blocchi #ifdef CONFIG_CTRL_IFACE_UDP (non usato)
 *  - Rimosso blocco #ifdef ANDROID (non usato)
 *  - Mantenuti tutti gli event string define (utili per parsing eventi)
 *  - Mantenuta dichiarazione wpa_ctrl_open2 (utile per cli_path custom)
 */

#ifndef WPA_CTRL_H
#define WPA_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* size_t */

/* =========================================================================
 * String define per gli eventi wpa_supplicant
 *
 * Usare con strncmp(buf, WPA_EVENT_XXX, strlen(WPA_EVENT_XXX)) per
 * identificare il tipo di evento ricevuto tramite wpa_ctrl_recv().
 * Nota: ogni stringa termina con uno spazio per evitare falsi match.
 * ========================================================================= */

/* Richieste interattive (wpa_supplicant chiede input all'utente) */
#define WPA_CTRL_REQ                        "CTRL-REQ-"
#define WPA_CTRL_RSP                        "CTRL-RSP-"

/* --- Eventi di connessione --- */
#define WPA_EVENT_CONNECTED                 "CTRL-EVENT-CONNECTED "
#define WPA_EVENT_DISCONNECTED              "CTRL-EVENT-DISCONNECTED "
#define WPA_EVENT_ASSOC_REJECT              "CTRL-EVENT-ASSOC-REJECT "
#define WPA_EVENT_AUTH_REJECT               "CTRL-EVENT-AUTH-REJECT "
#define WPA_EVENT_TERMINATING               "CTRL-EVENT-TERMINATING "

/* --- Password / EAP --- */
#define WPA_EVENT_PASSWORD_CHANGED          "CTRL-EVENT-PASSWORD-CHANGED "
#define WPA_EVENT_EAP_NOTIFICATION          "CTRL-EVENT-EAP-NOTIFICATION "
#define WPA_EVENT_EAP_STARTED               "CTRL-EVENT-EAP-STARTED "
#define WPA_EVENT_EAP_PROPOSED_METHOD       "CTRL-EVENT-EAP-PROPOSED-METHOD "
#define WPA_EVENT_EAP_METHOD                "CTRL-EVENT-EAP-METHOD "
#define WPA_EVENT_EAP_PEER_CERT             "CTRL-EVENT-EAP-PEER-CERT "
#define WPA_EVENT_EAP_PEER_ALT              "CTRL-EVENT-EAP-PEER-ALT "
#define WPA_EVENT_EAP_TLS_CERT_ERROR        "CTRL-EVENT-EAP-TLS-CERT-ERROR "
#define WPA_EVENT_EAP_STATUS                "CTRL-EVENT-EAP-STATUS "
#define WPA_EVENT_EAP_RETRANSMIT            "CTRL-EVENT-EAP-RETRANSMIT "
#define WPA_EVENT_EAP_SUCCESS               "CTRL-EVENT-EAP-SUCCESS "
#define WPA_EVENT_EAP_FAILURE               "CTRL-EVENT-EAP-FAILURE "
#define WPA_EVENT_EAP_TIMEOUT_FAILURE       "CTRL-EVENT-EAP-TIMEOUT-FAILURE "

/* --- Rete temporaneamente disabilitata --- */
#define WPA_EVENT_TEMP_DISABLED             "CTRL-EVENT-SSID-TEMP-DISABLED "
#define WPA_EVENT_REENABLED                 "CTRL-EVENT-SSID-REENABLED "

/* --- Scansione --- */
#define WPA_EVENT_SCAN_STARTED              "CTRL-EVENT-SCAN-STARTED "
#define WPA_EVENT_SCAN_RESULTS              "CTRL-EVENT-SCAN-RESULTS "
#define WPA_EVENT_SCAN_FAILED               "CTRL-EVENT-SCAN-FAILED "

/* --- Cambio stato --- */
#define WPA_EVENT_STATE_CHANGE              "CTRL-EVENT-STATE-CHANGE "

/* --- BSS (Access Point) --- */
#define WPA_EVENT_BSS_ADDED                 "CTRL-EVENT-BSS-ADDED "
#define WPA_EVENT_BSS_REMOVED               "CTRL-EVENT-BSS-REMOVED "

/* --- Segnale / qualità --- */
#define WPA_EVENT_NETWORK_NOT_FOUND         "CTRL-EVENT-NETWORK-NOT-FOUND "
#define WPA_EVENT_SIGNAL_CHANGE             "CTRL-EVENT-SIGNAL-CHANGE "
#define WPA_EVENT_BEACON_LOSS               "CTRL-EVENT-BEACON-LOSS "

/* --- Cambio canale / regione --- */
#define WPA_EVENT_REGDOM_CHANGE             "CTRL-EVENT-REGDOM-CHANGE "
#define WPA_EVENT_CHANNEL_SWITCH_STARTED    "CTRL-EVENT-STARTED-CHANNEL-SWITCH "
#define WPA_EVENT_CHANNEL_SWITCH            "CTRL-EVENT-CHANNEL-SWITCH "

/* --- SAE --- */
#define WPA_EVENT_SAE_UNKNOWN_PASSWORD_IDENTIFIER "CTRL-EVENT-SAE-UNKNOWN-PASSWORD-IDENTIFIER "

/* --- Subnet roaming --- */
#define WPA_EVENT_SUBNET_STATUS_UPDATE      "CTRL-EVENT-SUBNET-STATUS-UPDATE "

/* --- Frequenze da evitare --- */
#define WPA_EVENT_FREQ_CONFLICT             "CTRL-EVENT-FREQ-CONFLICT "
#define WPA_EVENT_AVOID_FREQ                "CTRL-EVENT-AVOID-FREQ "

/* =========================================================================
 * WPS
 * ========================================================================= */
#define WPS_EVENT_OVERLAP                   "WPS-OVERLAP-DETECTED "
#define WPS_EVENT_AP_AVAILABLE_PBC          "WPS-AP-AVAILABLE-PBC "
#define WPS_EVENT_AP_AVAILABLE_AUTH         "WPS-AP-AVAILABLE-AUTH "
#define WPS_EVENT_AP_AVAILABLE_PIN          "WPS-AP-AVAILABLE-PIN "
#define WPS_EVENT_AP_AVAILABLE              "WPS-AP-AVAILABLE "
#define WPS_EVENT_CRED_RECEIVED             "WPS-CRED-RECEIVED "
#define WPS_EVENT_M2D                       "WPS-M2D "
#define WPS_EVENT_FAIL                      "WPS-FAIL "
#define WPS_EVENT_SUCCESS                   "WPS-SUCCESS "
#define WPS_EVENT_TIMEOUT                   "WPS-TIMEOUT "
#define WPS_EVENT_ACTIVE                    "WPS-PBC-ACTIVE "
#define WPS_EVENT_DISABLE                   "WPS-PBC-DISABLE "
#define WPS_EVENT_ENROLLEE_SEEN             "WPS-ENROLLEE-SEEN "
#define WPS_EVENT_OPEN_NETWORK              "WPS-OPEN-NETWORK "

/* =========================================================================
 * DPP (Device Provisioning Protocol)
 * ========================================================================= */
#define DPP_EVENT_AUTH_SUCCESS              "DPP-AUTH-SUCCESS "
#define DPP_EVENT_AUTH_INIT_FAILED          "DPP-AUTH-INIT-FAILED "
#define DPP_EVENT_NOT_COMPATIBLE            "DPP-NOT-COMPATIBLE "
#define DPP_EVENT_CONF_RECEIVED             "DPP-CONF-RECEIVED "
#define DPP_EVENT_CONF_SENT                 "DPP-CONF-SENT "
#define DPP_EVENT_CONF_FAILED               "DPP-CONF-FAILED "
#define DPP_EVENT_CONFOBJ_SSID              "DPP-CONFOBJ-SSID "
#define DPP_EVENT_CONNECTOR                 "DPP-CONNECTOR "
#define DPP_EVENT_FAIL                      "DPP-FAIL "
#define DPP_EVENT_RX                        "DPP-RX "
#define DPP_EVENT_TX                        "DPP-TX "

/* =========================================================================
 * Mesh
 * ========================================================================= */
#define MESH_GROUP_STARTED                  "MESH-GROUP-STARTED "
#define MESH_GROUP_REMOVED                  "MESH-GROUP-REMOVED "
#define MESH_PEER_CONNECTED                 "MESH-PEER-CONNECTED "
#define MESH_PEER_DISCONNECTED              "MESH-PEER-DISCONNECTED "
#define MESH_SAE_AUTH_FAILURE               "MESH-SAE-AUTH-FAILURE "

/* =========================================================================
 * hostapd - eventi lato Access Point
 * ========================================================================= */
#define WPS_EVENT_PIN_NEEDED                "WPS-PIN-NEEDED "
#define WPS_EVENT_NEW_AP_SETTINGS           "WPS-NEW-AP-SETTINGS "
#define WPS_EVENT_REG_SUCCESS               "WPS-REG-SUCCESS "
#define WPS_EVENT_AP_SETUP_LOCKED           "WPS-AP-SETUP-LOCKED "
#define WPS_EVENT_AP_SETUP_UNLOCKED         "WPS-AP-SETUP-UNLOCKED "
#define AP_STA_CONNECTED                    "AP-STA-CONNECTED "
#define AP_STA_DISCONNECTED                 "AP-STA-DISCONNECTED "
#define AP_STA_POSSIBLE_PSK_MISMATCH        "AP-STA-POSSIBLE-PSK-MISMATCH "
#define AP_EVENT_ENABLED                    "AP-ENABLED "
#define AP_EVENT_DISABLED                   "AP-DISABLED "
#define INTERFACE_ENABLED                   "INTERFACE-ENABLED "
#define INTERFACE_DISABLED                  "INTERFACE-DISABLED "

/* =========================================================================
 * DFS / radar
 * ========================================================================= */
#define DFS_EVENT_RADAR_DETECTED            "DFS-RADAR-DETECTED "
#define DFS_EVENT_NEW_CHANNEL               "DFS-NEW-CHANNEL "
#define DFS_EVENT_CAC_START                 "DFS-CAC-START "
#define DFS_EVENT_CAC_COMPLETED             "DFS-CAC-COMPLETED "
#define DFS_EVENT_NOP_FINISHED              "DFS-NOP-FINISHED "

/* =========================================================================
 * BSS command bitmask (per il comando "BSS")
 * ========================================================================= */
#define WPA_BSS_MASK_ALL            0xFFFDFFFF
#define WPA_BSS_MASK_ID             (1u << 0)
#define WPA_BSS_MASK_BSSID          (1u << 1)
#define WPA_BSS_MASK_FREQ           (1u << 2)
#define WPA_BSS_MASK_BEACON_INT     (1u << 3)
#define WPA_BSS_MASK_CAPABILITIES   (1u << 4)
#define WPA_BSS_MASK_QUAL           (1u << 5)
#define WPA_BSS_MASK_NOISE          (1u << 6)
#define WPA_BSS_MASK_LEVEL          (1u << 7)
#define WPA_BSS_MASK_TSF            (1u << 8)
#define WPA_BSS_MASK_AGE            (1u << 9)
#define WPA_BSS_MASK_IE             (1u << 10)
#define WPA_BSS_MASK_FLAGS          (1u << 11)
#define WPA_BSS_MASK_SSID           (1u << 12)
#define WPA_BSS_MASK_WPS_SCAN       (1u << 13)
#define WPA_BSS_MASK_P2P_SCAN       (1u << 14)
#define WPA_BSS_MASK_INTERNETW      (1u << 15)
#define WPA_BSS_MASK_WIFI_DISPLAY   (1u << 16)
#define WPA_BSS_MASK_DELIM          (1u << 17)
#define WPA_BSS_MASK_MESH_SCAN      (1u << 18)
#define WPA_BSS_MASK_SNR            (1u << 19)
#define WPA_BSS_MASK_EST_THROUGHPUT (1u << 20)
#define WPA_BSS_MASK_BEACON_IE      (1u << 23)

/* =========================================================================
 * API pubblica
 * ========================================================================= */

/**
 * wpa_ctrl_open - Apre una connessione al socket di controllo wpa_supplicant
 * @ctrl_path: Path al socket, es. "/var/run/wpa_supplicant/wlan0"
 * Returns: handle opaco o NULL su errore
 */
struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path);

/**
 * wpa_ctrl_open2 - Apre una connessione specificando anche il path del socket client
 * @ctrl_path: Path al socket wpa_supplicant
 * @cli_path:  Directory per il socket client (default: /tmp se NULL)
 * Returns: handle opaco o NULL su errore
 */
struct wpa_ctrl *wpa_ctrl_open2(const char *ctrl_path, const char *cli_path);

/**
 * wpa_ctrl_close - Chiude la connessione e libera le risorse
 * @ctrl: Handle da wpa_ctrl_open()
 */
void wpa_ctrl_close(struct wpa_ctrl *ctrl);

/**
 * wpa_ctrl_request - Invia un comando e attende la risposta (BLOCCANTE)
 * @ctrl:      Handle connessione comandi (cmd_ctrl, NON mon_ctrl)
 * @cmd:       Stringa comando, es. "SCAN" o "STATUS"
 * @cmd_len:   Lunghezza di cmd
 * @reply:     Buffer per la risposta
 * @reply_len: In ingresso: dimensione buffer; In uscita: byte ricevuti
 * @msg_cb:    Callback per messaggi asincroni intercettati durante l'attesa
 *             (può essere NULL)
 * Returns: 0 ok, -1 errore I/O, -2 timeout
 */
int wpa_ctrl_request(struct wpa_ctrl *ctrl,
		     const char *cmd, size_t cmd_len,
		     char *reply, size_t *reply_len,
		     void (*msg_cb)(char *msg, size_t len));

/**
 * wpa_ctrl_attach - Registra la connessione come monitor eventi
 * @ctrl: Handle connessione monitor (mon_ctrl)
 * Returns: 0 ok, -1 errore, -2 timeout
 *
 * Dopo questa chiamata, wpa_supplicant inizierà a inviare eventi
 * su questa connessione. Leggere con wpa_ctrl_recv().
 */
int wpa_ctrl_attach(struct wpa_ctrl *ctrl);

/**
 * wpa_ctrl_detach - Deregistra la connessione dal monitor eventi
 * @ctrl: Handle connessione monitor
 * Returns: 0 ok, -1 errore, -2 timeout
 */
int wpa_ctrl_detach(struct wpa_ctrl *ctrl);

/**
 * wpa_ctrl_recv - Legge un messaggio evento dalla connessione monitor
 * @ctrl:      Handle connessione monitor (dopo wpa_ctrl_attach)
 * @reply:     Buffer per il messaggio
 * @reply_len: In ingresso: dimensione buffer; In uscita: byte ricevuti
 * Returns: 0 ok, -1 errore (errno=EAGAIN se nessun dato disponibile)
 *
 * Chiamare solo dopo wpa_ctrl_pending() > 0 o dopo che poll/select
 * ha segnalato il fd come leggibile.
 */
int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len);

/**
 * wpa_ctrl_pending - Controlla se ci sono eventi in attesa (non bloccante)
 * @ctrl: Handle connessione monitor
 * Returns: 1 se ci sono dati, 0 se nessun dato, -1 su errore
 *
 */
int wpa_ctrl_pending(struct wpa_ctrl *ctrl);

/**
 * wpa_ctrl_get_fd - Ritorna il file descriptor del socket
 * @ctrl: Handle connessione
 * Returns: fd del socket UNIX
 *
 */
int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* WPA_CTRL_H */