#include "../../logic.h"
#include "bthLogic.h"

/* =====================
   VARIABILI GLOBALI
======================== */
static GDBusConnection* bt_conn = NULL;             // Variabile per connessione al bus del sistema
static GDBusObjectManager* bt_manager = NULL;       // Variabile per Object Manager
static GDBusObject* bt_adapter_obj = NULL;          // Variabile per adapter per comunicazione con DBUS
static gboolean adapter_original_powered = FALSE;   // Variabile per sapere lo stato di accensione dell'adapter originale
static gulong device_added_handler_id = 0;          // Variabile che identifica l'ID del segnale collegato
static gboolean scanning = FALSE;                   // Variabile per verificare che una scan sia in corso prima di provare a fermarla
static gboolean pairing_in_progress = FALSE;        // Variabile per tenere traccia se siamo in un processo di pairing in corso
static gboolean unpairing_in_progress = FALSE;      // Variabile per tenere traccia se siamo in un processo di unpairing in corso

/* Array condiviso con per popolare lista*/
static NetDevice devices[MAX_DEVICES];              // Array di device trovati
static int device_count = 0;                        // Numero di device trovati

/* --- Definizione dell'interfaccia Agent --- */
static const gchar agent_introspection_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "    <method name='Release'/>"
    "  </interface>"
    "</node>";

static guint agent_reg_id = 0;                      // ID per g_dbus_connection_register_object, usato per deregistrare l'agent al termine

/* --- Watch PropertiesChanged per il device in pairing --- */
static GDBusProxy* bt_pairing_proxy  = NULL;        // Proxy tenuto vivo durante il pairing per ascoltare PropertiesChanged
static gulong      bt_pairing_sig_id = 0;           // ID del segnale g-properties-changed collegato al proxy di pairing
static gulong      bt_device_removed_sig_id = 0;    // ID del segnale object-removed sull'ObjectManager per rilevare rimozione device (es. dopo unpair)

/* --- Timer di timeout per la UI di pairing --- */
#define BT_PAIRING_UI_TIMEOUT_MS 10000              // 10s: feedback all'utente se RequestConfirmation non arriva
static lv_timer_t *bt_pairing_timer = NULL;         // Timer LVGL attivo durante il pairing; NULL se inattivo

/* =====================
   PROTOTIPI
======================== */

static void device_added_cb(GDBusObjectManager* manager, GDBusObject* object, gpointer user_data);                              // Funzione di callback per quando viene trovato un dispositivo
static gboolean find_first_adapter(void);                                                                                       // Funzione per trovare il primo Adapter disponibile per comunicazione con DBus
static void stop_scan(void);                                                                                                    // Funzione per fermare la scansione
static gboolean alias_equals_addr(const char* alias, const char* addr);                                                         // Funzione per vedere se l'alias trovato è uguale al MAC address
static void load_paired_devices(void);                                                                                          // Funzione per inserire nell'array i dispositivi con cui ho già fatto pairing
static void register_bth_agent(void);                                                                                           // Funzione per registrare l'agent bth per il pairing
static void bt_pairing_success_cb(void *p);                                                                                     // Funzione di callback chiamata quando il pairing è effettivamente riuscito con successo per eseguire operazioni LVGL in modo thread-safe
static void on_pairing_finished(GObject *source_object, GAsyncResult *res, gpointer user_data);                                 // Funzione di callback chiamata quando il pairing è finito (dopo click utente sulla message box)
static void on_remove_finished(GObject *source_object, GAsyncResult *res, gpointer user_data);                                  // Funzione di callback chiamata quando RemoveDevice è terminato. Ha bisogno del MAC address del device per identificare quale device è stato unpaired, passato in user_data
static void on_device_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer user_data);          // Listener PropertiesChanged: aggiorna connected nell'array quando BlueZ conferma Paired. Ha bisogno del MAC address per identificare il device, passato in user_data                                
static void on_device_removed_cb(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);                         // Listener object-removed: aggiorna connected=FALSE quando BlueZ rimuove il device. Ha bisogno del MAC address per identificare il device rimosso, passato in user_data
static void bt_stop_pairing_watch(void);                                                                                        // Disconnette e libera il proxy watch del pairing corrente
static void bt_pairing_timeout_cb(lv_timer_t *timer);                                                                           // Callback del timer LVGL: scatta se RequestConfirmation non arriva entro BT_PAIRING_UI_TIMEOUT_MS
static void bt_stop_pairing_timer(void);                                                                                        // Cancella il timer di pairing in modo sicuro (no-op se non attivo)

// Funzione di callback per la gestione delle chiamate al nostro agent (conferma pairing, autorizzazione servizio...)
static void handle_agent_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                                     const gchar *iface, const gchar *method, GVariant *params,
                                     GDBusMethodInvocation *invocation, gpointer user_data);              

/* --- VTable per l'agent, in modo da dirgli che funzione chiamare quando contattato da DBus --- */
static const GDBusInterfaceVTable agent_vtable = { .method_call = handle_agent_method_call };

/* ===========================
   IMPLEMENTAZIONI
============================== */

/* -------------------------------------------------------------------------
 * UTILITIES
 * ------------------------------------------------------------------------- */

gboolean alias_equals_addr(const char* alias, const char* addr)
{
    if (!alias || !addr) return FALSE;

    int i = 0, j = 0;
    while (alias[i] && addr[j])
    {
        if (alias[i] == '-' && addr[j] == ':') { i++; j++; continue; }
        if (alias[i] != addr[j]) return FALSE;
        i++; j++;
    }
    return (alias[i] == '\0' && addr[j] == '\0');
}

void device_added_cb(GDBusObjectManager* manager, GDBusObject* object, gpointer user_data)
{
    GDBusInterface* iface = g_dbus_object_get_interface(object, "org.bluez.Device1");
    if (!iface)
        return;

    GDBusProxy* device_proxy = G_DBUS_PROXY(iface);

    GVariant* addr_var  = g_dbus_proxy_get_cached_property(device_proxy, "Address");
    GVariant* name_var  = g_dbus_proxy_get_cached_property(device_proxy, "Name");
    GVariant* alias_var = g_dbus_proxy_get_cached_property(device_proxy, "Alias");
    GVariant* paired_var    = g_dbus_proxy_get_cached_property(device_proxy, "Paired");
    GVariant* rssi_var      = g_dbus_proxy_get_cached_property(device_proxy, "RSSI");    

    gchar* addr = NULL;
    gchar* name = NULL;

    /* ---  Paired / RSSI --- */
    gboolean is_paired    = paired_var    ? g_variant_get_boolean(paired_var)    : FALSE;
    int16_t rssi_val      = rssi_var      ? g_variant_get_int16(rssi_var)        : 0;

    /* --- Address --- */
    if (addr_var)
        addr = g_strdup(g_variant_get_string(addr_var, NULL));

    /* --- Name / Alias --- */
    if (name_var)
    {
        const char* tmp = g_variant_get_string(name_var, NULL);
        if (tmp)
            name = g_strdup(tmp);
    }
    else if (alias_var && addr)
    {
        const char* alias_tmp = g_variant_get_string(alias_var, NULL);
        if (alias_tmp && !alias_equals_addr(alias_tmp, addr))
            name = g_strdup(alias_tmp);
    }
    if (!name && is_paired && addr) 
    {
        // Se non ho un nome ma è paired, uso l'indirizzo come nome (per evitare di avere solo "Unknown" nella lista dei paired)
        name = g_strdup(addr); // oppure "Paired device"
    }

    /* --- Se non valido per la UI, non creo il bottone --- */
    /* 
    * Device discovered:
    *  - deve avere Name valido
    * Device paired:
    *  - Name può mancare
    */
    if (!addr)
    goto cleanup;

    if (!is_paired) 
    {
        // device discovered
        if (!name || strcmp(name, "Unknown") == 0)
            goto cleanup;
    }

    /* --- Check duplicati --- */
    for (int i = 0; i < device_count; i++)
    {
        if (strcmp(devices[i].address, addr) == 0)
            goto cleanup;
    }

    /* --- Aggiunta tra i dispositivi validi trovati se ho ancora spazio --- */
    if (device_count < MAX_DEVICES)
    {
        // Copia nome e address
        strncpy(devices[device_count].name, name, DEVICE_NAME_LEN - 1);
        devices[device_count].name[DEVICE_NAME_LEN - 1] = '\0';

        strncpy(devices[device_count].address, addr, DEVICE_ADDR_LEN - 1);
        devices[device_count].address[DEVICE_ADDR_LEN - 1] = '\0';

        // Paired, RSSI
        devices[device_count].connected = is_paired;
        devices[device_count].rssi      = rssi_val;

        // Aumenta il numero di dispositivi validi
        device_count++;

        // Aggiorna UI (async-safe)
        createDeviceList(devices, device_count);
    }

cleanup:
    if (addr)  g_free(addr);
    if (name)  g_free(name);

    if (addr_var)  g_variant_unref(addr_var);
    if (name_var)  g_variant_unref(name_var);
    if (alias_var) g_variant_unref(alias_var);
    if (paired_var) g_variant_unref(paired_var);
    if (rssi_var) g_variant_unref(rssi_var);

    g_object_unref(iface);
}

gboolean find_first_adapter()
{
    GList* objects = g_dbus_object_manager_get_objects(bt_manager);
    for (GList* l = objects; l != NULL; l = l->next) 
    {
        GDBusObject* obj = G_DBUS_OBJECT(l->data);
        GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.Adapter1");
        if (iface) 
        {
            bt_adapter_obj = g_object_ref(obj);

            // Creo il proxy per leggere/modificare proprietà
            GError* error = NULL;
            GDBusProxy* adapter_proxy = g_dbus_proxy_new_sync(
                bt_conn,
                G_DBUS_PROXY_FLAGS_NONE,
                NULL,
                "org.bluez",
                g_dbus_object_get_object_path(obj),
                "org.bluez.Adapter1",
                NULL,
                &error
            );

            if (!adapter_proxy) 
            {
                ERROR_PRINT("Errore creazione Adapter Proxy: %s\n", error ? error->message : "Unknown");
                if (error) g_error_free(error);
                g_object_unref(iface);
                g_list_free_full(objects, g_object_unref);

                return FALSE;
            }

            // Leggo proprietà Powered e Discoverable
            GVariant* powered_var = g_dbus_proxy_get_cached_property(adapter_proxy, "Powered");
            GVariant* discoverable_var = g_dbus_proxy_get_cached_property(adapter_proxy, "Discoverable");

            gboolean powered = powered_var ? g_variant_get_boolean(powered_var) : FALSE;
            gboolean discoverable = discoverable_var ? g_variant_get_boolean(discoverable_var) : FALSE;

            // Mi salvo qual'era lo stato originale dell'adapter
            adapter_original_powered = powered;


            INFO_PRINT("Adapter trovato: %s\n", g_dbus_object_get_object_path(obj));
            INFO_PRINT("Powered=%s Discoverable=%s\n", powered ? "TRUE" : "FALSE", discoverable ? "TRUE" : "FALSE");

            // Se spento, accendo l'adapter
            if (!powered) 
            {
                GError* power_error = NULL;
                g_dbus_proxy_call_sync(
                    adapter_proxy,
                    "org.freedesktop.DBus.Properties.Set",
                    g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)),
                    G_DBUS_CALL_FLAGS_NONE,
                    -1,
                    NULL,
                    &power_error
                );

                if (power_error) {
                    ERROR_PRINT("Errore accensione adapter: %s\n", power_error->message);
                    g_error_free(power_error);
                } else {
                    DEBUG_PRINT("Adapter acceso con successo.\n");
                    // sleep per dare tempo all'adapter di accendersi
                    g_usleep(200 * 1000); // 200ms
                }
            }


            if (powered_var) g_variant_unref(powered_var);
            if (discoverable_var) g_variant_unref(discoverable_var);
            g_object_unref(adapter_proxy);
            g_object_unref(iface);
            g_list_free_full(objects, g_object_unref);
            return TRUE;
        }
    }

    g_list_free_full(objects, g_object_unref);
    return FALSE;
}

/* -------------------------------------------------------------------------
 * CONNECT / DISCONNECT 
 * ------------------------------------------------------------------------- */

void handle_agent_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer user_data)
{
    if (g_strcmp0(method, "RequestConfirmation") == 0) 
    {
        const gchar *dev_path;
        guint32 passkey;
        g_variant_get(params, "(&ou)", &dev_path, &passkey);

        // Allochiamo la struct (verrà liberata nel wrapper)
        pairing_popup_data_t *data = g_malloc(sizeof(pairing_popup_data_t));
        data->dev_path = g_strdup(dev_path);
        data->passkey = passkey;
        data->invocation = g_object_ref(invocation); // Incrementiamo il riferimento perché l'invocation è valida finché non rispondiamo, e noi risponderemo alla fine della UI di pairing, non ora

        // La funzione di creazione della MessageBox è async-safe, quindi possiamo chiamarla direttamente da qui passando i dati necessari tramite la struct
        lv_async_call(ui_show_pairing_popup_wrapper, data);
    } 
    else if (g_strcmp0(method, "AuthorizeService") == 0) 
    {
        // Se non hai servizi, autorizziamo tutto di default per permettere il pairing
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method, "Cancel") == 0)
    {
        // Cancel non richiede risposta, ma dobbiamo gestirlo per evitare di lasciare invocation pendenti se BlueZ annulla il pairing (Es: Utente clicca "Annulla" o dispositivo esce da portata)
        DEBUG_PRINT("Agent: Cancel ricevuto da BlueZ.\n");

        // Rispondiamo a BlueZ
        g_dbus_method_invocation_return_value(invocation, NULL);

        // Pairing fallisce se in corso
        if (pairing_in_progress) 
        {
            pairing_in_progress = FALSE;

            // Aggiorno la UI async-safe per mostrare il fallimento del pairing
            lv_async_call(connectionFailed, NULL);

            // Puliamo l'invocazione (Sblocca il mutex e mette a NULL)
            bt_clear_pending_invocation("org.bluez.Error.Canceled", "Pairing annullato da BlueZ");

            // Fermiamo il timer di pairing se è ancora attivo
            bt_stop_pairing_timer();

            // Liberiamo il watch di PropertiesChanged: il pairing è fallito, non serve più ascoltare
            bt_stop_pairing_watch();

            ui_log_async("Pairing annullato da BlueZ.");
        }
    }
    else if (g_strcmp0(method, "Release") == 0)
    {
        // Rispondiamo ad eventuale invocation pendente con errore Canceled per indicare che il pairing è stato annullato perché l'agent è stato rilasciato
        DEBUG_PRINT("Agent: Release ricevuto da BlueZ.\n");

        // Se pairing in corso, consideriamolo fallito a causa del rilascio dell'agent
        if (pairing_in_progress) 
        {
            pairing_in_progress = FALSE;

            // Rispondiamo a BlueZ che il pairing è stato annullato a causa del rilascio dell'agent
            bt_clear_pending_invocation("org.bluez.Error.Canceled", "Pairing annullato: Agent rilasciato");

            // Aggiorno la UI async-safe per mostrare il fallimento del pairing
            lv_async_call(connectionFailed, NULL);

            // Fermiamo il timer di pairing se è ancora attivo
            bt_stop_pairing_timer();

            // Liberiamo il watch di PropertiesChanged: il pairing è fallito, non serve più ascoltare
            bt_stop_pairing_watch();

            ui_log_async("Pairing annullato: Agent rilasciato.");
        }
    }
    else
    {
        DEBUG_PRINT("Agent: Metodo sconosciuto ricevuto: %s\n", method);
        
        /* Metodo sconosciuto: rispondiamo con errore per non bloccare BlueZ */
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.UnknownMethod", "Metodo sconosciuto");
 
        // Se è in corso il pairing, lo considero fallito
        if (pairing_in_progress) 
        {
            pairing_in_progress = FALSE;

            // Metodo sconosciuto: rispondiamo con errore per non bloccare BlueZ
            bt_clear_pending_invocation("org.bluez.Error.UnknownMethod", "Metodo sconosciuto");
    
            // Aggiorno la UI async-safe per mostrare il fallimento del pairing
            lv_async_call(connectionFailed, NULL);

            // Fermiamo il timer di pairing se è ancora attivo
            bt_stop_pairing_timer();

            // Liberiamo il watch di PropertiesChanged: il pairing è fallito, non serve più ascoltare
            bt_stop_pairing_watch();

            ui_log_async("Pairing fallito: Metodo sconosciuto ricevuto dall'agent.");
        }
    }
}

void on_pairing_finished(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    
    // Recuperiamo l'indirizzo che avevamo passato come user_data
    char* device_addr = (char*)user_data;

    if (error) 
    {
        ERROR_PRINT("Errore D-Bus Pair(): %s\n", error->message);
        
        if (pairing_in_progress) 
        {
            pairing_in_progress = FALSE;

            ui_log_async("Pairing fallito: %s", error->message);

            // Aggiorno la UI async-safe per mostrare il fallimento del pairing
            lv_async_call(connectionFailed, NULL);

            // Rispondiamo a BlueZ che il pairing è fallito a causa di un errore D-Bus
            bt_clear_pending_invocation("org.bluez.Error.Failed", "Pairing fallito a causa di un errore D-Bus");

            // Liberiamo il watch di PropertiesChanged: il pairing è fallito, non serve più ascoltare
            bt_stop_pairing_watch();
        }

        g_error_free(error);
    }
    else
    {
        DEBUG_PRINT("Pairing completato con successo!\n");
        ui_log_async("Dispositivo abbinato correttamente.");
        g_variant_unref(result);
    }

    // Cancelliamo il timer di pairing se non è ancora scattato
    bt_stop_pairing_timer();

    // Liberiamo la stringa dell'indirizzo duplicata
    if (device_addr) g_free(device_addr);
}

void on_remove_finished(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    char *device_addr = (char *)user_data;

    if (error) 
    {
        // Gestiamo l'errore di RemoveDevice: mostriamo un messaggio di errore 
        ERROR_PRINT("Errore RemoveDevice: %s\n", error->message);
        ui_log_async("Unpairing fallito: %s", error->message);
        
        g_error_free(error);

        // Aggiotno la UI in modo async-safe per riflettere il fallimento dell'unpairing
        lv_async_call(disconnectionFailed, NULL); 
    } 
    else 
    {
        // RemoveDevice ha avuto successo: mostriamo un messaggio di successo. La UI verrà aggiornata da on_device_removed_cb quando BlueZ rimuoverà il device e scatterà object-removed
        DEBUG_PRINT("RemoveDevice call success for %s", device_addr);
        g_variant_unref(result);
        
        // Comunichiamo la UI async-safe per mostrare il device come disconnesso 
        lv_async_call(disconnectionSuccess, NULL);
    }

    // Aggiorno la logica per indicare che non siamo più in unpairing, indipendentemente dall'esito
    unpairing_in_progress = FALSE;

    if (device_addr) g_free(device_addr);
    g_object_unref(G_DBUS_PROXY(source_object));
}

int bth_connect_to(NetDevice device)
{
    /*--------------- PASSO 1: PREPARAZIONE DEL DBUS ---------------*/
    INFO_PRINT("Pairing a %s [%s]...\n", device.name, device.address);

    if (!bt_conn || !bt_manager || !bt_adapter_obj) {
        ui_log_async("Errore: Bluetooth non inizializzato correttamente.\n");
        ERROR_PRINT("Errore: Bluetooth non inizializzato correttamente.\n");
        return -1;
    }

    // Creo il path DBus del device: org/bluez/hciX/dev_XX_XX_XX_XX_XX_XX
    char dev_path[128];
    snprintf(dev_path, sizeof(dev_path), "%s/dev_%s",
             g_dbus_object_get_object_path(bt_adapter_obj),
             device.address);
    // sostituire ':' con '_'
    for (int i = 0; dev_path[i]; i++)
        if (dev_path[i] == ':') dev_path[i] = '_';

    GError* error = NULL;
    GDBusProxy* device_proxy = g_dbus_proxy_new_sync(
        bt_conn,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.bluez",
        dev_path,
        "org.bluez.Device1",
        NULL,
        &error
    );

    if (!device_proxy) {
        ui_log_async("Errore creazione proxy device: %s\n", error ? error->message : "Unknown");
        ERROR_PRINT("Errore creazione proxy device: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return -1;
    }

    // Recupero proprietà base (Paired)
    GVariant* paired_var = g_dbus_proxy_get_cached_property(device_proxy, "Paired");
    gboolean is_paired = paired_var ? g_variant_get_boolean(paired_var) : FALSE;
    if (paired_var) g_variant_unref(paired_var);

    /*--------------- PASSO 2: PAIRING SE NON GIA EFFETTUATO ---------------*/
    if (!is_paired)
    {
        DEBUG_PRINT("Avvio pairing asincrono per %s...\n", device.address);

        // Fermiamo eventuali watch di pairing precedenti per evitare interferenze con nuovi pairing
        bt_stop_pairing_watch();

        // Fermiamo eventuale timer di pairing precedente per evitare che scatti durante un nuovo pairing
        bt_stop_pairing_timer();

        // Colleghiamo il listener PropertiesChanged al proxy del device in pairing
        char *addr_copy = g_strdup(device.address);
        bt_pairing_proxy  = device_proxy; /* trasferimento ownership */
        bt_pairing_sig_id = g_signal_connect_data(
            bt_pairing_proxy,
            "g-properties-changed",
            G_CALLBACK(on_device_properties_changed),
            addr_copy,              /* user_data */
            (GClosureNotify)g_free, /* destroy_notify: libera addr_copy quando il segnale è disconnesso */
            0                       
        );

        // Chiamata asincrona a Pair() per avviare il processo di pairing 
        g_dbus_proxy_call(
            bt_pairing_proxy,
            "Pair",
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            60000,
            NULL,
            on_pairing_finished,
            g_strdup(device.address)
        );

        // Avviamo il timer UI, il tempo max in cui ci aspettiamo che BlueZ chiami RequestConfirmation dopo Pair()
        bt_pairing_timer = lv_timer_create(bt_pairing_timeout_cb, BT_PAIRING_UI_TIMEOUT_MS, NULL);
        lv_timer_set_repeat_count(bt_pairing_timer, 1);

        // Aggiorno la logica per indicare che siamo in pairing 
        pairing_in_progress = TRUE;

        // Ritorniamo subito: l'esito del pairing arriverà in on_pairing_finished e on_device_properties_changed
        return 0; 
    }

    /*--------------- PASSO 3: SE ERA GIÀ PAIRED ---------------*/
    // C'è chiesto di connettersi a un dispositivo già paired: aggiorniamo lo stato connected nell'array e nella UI, ma non chiamiamo Pair() perché è già paired (e BlueZ non lo permette)

    ui_log_async("Dispositivo già abbinato.");
    DEBUG_PRINT("Dispositivo già abbinato.\n");

    for (int i = 0; i < device_count; i++)
    {
        if (strcmp(devices[i].address, device.address) == 0)
        {
            devices[i].connected = TRUE;
            break;
        }
    }
    createDeviceList(devices, device_count);

    g_object_unref(device_proxy);

    // Aggiorno la logica per indicare che non siamo in pairing
    pairing_in_progress = FALSE;

    return 0;
}

int bth_disconnect(NetDevice device)
{
    /*--------------- PASSO 1: PREPARAZIONE DEL DBUS ---------------*/
    DEBUG_PRINT("Unpairing da da %s [%s]...\n", device.name, device.address);

    if (!bt_conn || !bt_manager || !bt_adapter_obj) {
        ui_log_async("Errore: Bluetooth non inizializzato correttamente.\n");
        ERROR_PRINT("Errore: Bluetooth non inizializzato correttamente.\n");
        return -1;
    }

    // Creo il path DBus del device: org/bluez/hciX/dev_XX_XX_XX_XX_XX_XX
    char dev_path[128];
    snprintf(dev_path, sizeof(dev_path), "%s/dev_%s",
             g_dbus_object_get_object_path(bt_adapter_obj),
             device.address);
    for (int i = 0; dev_path[i]; i++)
        if (dev_path[i] == ':') dev_path[i] = '_';

    /*--------------- PASSO 2: LETTURA VALORE PAIRING ---------------*/
    // Creo proxy del device per leggere proprietà
    GError* error = NULL;
    GDBusProxy* device_proxy = g_dbus_proxy_new_sync(
        bt_conn,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.bluez",
        dev_path,
        "org.bluez.Device1",
        NULL,
        &error
    );

    if (!device_proxy) {
        ui_log_async("ERRORE: in creazione proxy device per controllo pairing: %s\n", error ? error->message : "Unknown");
        ERROR_PRINT("ERRORE: in creazione proxy device per controllo pairing: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return -1;
    }

    // Controllo se è paired
    GVariant* paired_var = g_dbus_proxy_get_cached_property(device_proxy, "Paired");
    gboolean is_paired = paired_var ? g_variant_get_boolean(paired_var) : FALSE;
    if (paired_var) g_variant_unref(paired_var);

    if (!is_paired) 
    {
        g_object_unref(device_proxy);
        return 1; // Non è paired, quindi consideriamo già "disconnesso" e usciamo senza errori ma con codice specifico
    }

    g_object_unref(device_proxy); // Non serve più il proxy device

    /*--------------- PASSO 3: PREPARAZIONE DELL'ADAPTER PER RIMOZIONE PAIRING ---------------*/
    // Creo proxy dell'adapter per rimuovere il device
    GDBusProxy* adapter_proxy = g_dbus_proxy_new_sync(
        bt_conn,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.bluez",
        g_dbus_object_get_object_path(bt_adapter_obj),
        "org.bluez.Adapter1",
        NULL,
        &error
    );

    if (!adapter_proxy) 
    {
        ui_log_async("ERRORE: in creazione proxy adapter per rimozione device: %s\n", error ? error->message : "Unknown");
        ERROR_PRINT("ERRORE: in creazione proxy adapter per rimozione device: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return -1;
    }

    /*--------------- PASSO 4: RIMOZIONE PAIRING ASINCRONA ---------------*/
    /*
     * Usiamo la chiamata ASINCRONA per non bloccare il thread UI.
     * adapter_proxy NON viene liberato qui: ownership trasferita a
     * on_remove_finished, che lo libera con g_object_unref al termine.
     * device_addr (g_strdup) viene liberato anch'esso in on_remove_finished.
     */

    g_dbus_proxy_call(
        adapter_proxy,
        "RemoveDevice",
        g_variant_new("(o)", dev_path),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        on_remove_finished,
        g_strdup(device.address)
    );

    // Aggiorno la logica per indicare che siamo in unpairing
    unpairing_in_progress = TRUE;

    /* Ritorniamo subito: l'esito arriverà in on_remove_finished */
    return 0;
}

/* -------------------------------------------------------------------------
 * WATCH PROPERTIES CHANGED
 * ------------------------------------------------------------------------- */

void bt_stop_pairing_watch(void)
{
    if (bt_pairing_proxy != NULL)
    {
        if (bt_pairing_sig_id != 0)
        {
            g_signal_handler_disconnect(bt_pairing_proxy, bt_pairing_sig_id);
            bt_pairing_sig_id = 0;
        }
        g_object_unref(bt_pairing_proxy);
        bt_pairing_proxy = NULL;
    }
}

void bt_stop_pairing_timer(void)
{
    if (bt_pairing_timer != NULL)
    {
        lv_timer_delete(bt_pairing_timer);
        bt_pairing_timer = NULL;
    }
}

void bt_pairing_timeout_cb(lv_timer_t *timer)
{
    INFO_PRINT("Timeout pairing UI: nessuna RequestConfirmation ricevuta entro %d ms.\n", BT_PAIRING_UI_TIMEOUT_MS);
    ui_log_async("TIMEOUT PAIRING: nessuna risposta dal dispositivo.");

    // Annulliamo l'invocation pendente con errore Canceled per indicare che il pairing è stato annullato per timeout
    bt_clear_pending_invocation("org.bluez.Error.Canceled", "Timeout pairing UI");

    // Ferma il watch: il pairing non è andato a buon fine 
    bt_stop_pairing_watch();

    // Aggiorno la UI async-safe per mostrare il fallimento del pairing
    lv_async_call(connectionFailed, NULL);

    // Azzero il timer attivo: se è scattato, non c'è più un timer valido in giro
    bt_pairing_timer = NULL;

    // Aggiorno logica per indicare che non siamo più in pairing
    pairing_in_progress = FALSE;
}

void bt_pairing_success_cb(void *p)
{
    (void)p;
    bt_stop_pairing_timer();        // Pairing confermato: il timer LVGL non deve più scattare 
    pairing_in_progress = FALSE;    // Aggiorno la logica per indicare che il pairing è finito
    connectionSuccess(NULL);        // Aggiorno UI
}

void on_device_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer user_data)
{
    // Recuperiamo l'indirizzo del device in pairing passato come user_data
    const char *device_addr = (const char *)user_data;

    // Controlliamo se tra le proprietà cambiate c'è "Paired"
    GVariant *paired_var = g_variant_lookup_value(changed, "Paired", G_VARIANT_TYPE_BOOLEAN);
    if (!paired_var)
        return;

    // Leggiamo il nuovo valore di Paired
    gboolean is_paired = g_variant_get_boolean(paired_var);
    g_variant_unref(paired_var);

    DEBUG_PRINT("PropertiesChanged: Paired=%s per %s\n", is_paired ? "TRUE" : "FALSE", device_addr);

    // Aggiorniamo l'array dei dispositivi: se è il device in pairing, aggiorniamo connected al nuovo stato
    for (int i = 0; i < device_count; i++)
    {
        if (strcmp(devices[i].address, device_addr) == 0)
        {
            devices[i].connected = is_paired;
            break;
        }
    }

    // Paired confermato: fermiamo timer e wath | Unpaired: logghiamo la rimozione del pairing 
    if (is_paired)
    {
        // Sblocca eventuale invocation pendente senza errore, perché il pairing è andato a buon fine
        bt_clear_pending_invocation(NULL, NULL); 

        // Liberiamo il watch di PropertiesChanged: il pairing è confermato, non serve più ascoltare
        bt_stop_pairing_watch();        

        // Effettuiamo operazioni LVGL in modo thread-safe
        lv_async_call(bt_pairing_success_cb, NULL);
    }

    // Aggiorniamo la UI async-safe
    createDeviceList(devices, device_count);
}

void on_device_removed_cb(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    // Ottengo l'interfaccia Device1 per recuperare l'indirizzo del device rimosso
    GDBusInterface *iface = g_dbus_object_get_interface(object, "org.bluez.Device1");
    if (!iface)
        return;

    // Recupero l'indirizzo del device rimosso per identificare quale device è stato rimosso
    GDBusProxy *device_proxy = G_DBUS_PROXY(iface);
    GVariant *addr_var = g_dbus_proxy_get_cached_property(device_proxy, "Address");

    // Cerco il device nell'array per aggiornare lo stato connected a FALSE e aggiornare la UI
    if (addr_var)
    {
        const char *addr = g_variant_get_string(addr_var, NULL);

        // Quando faccio unpairing rimuovo sempre il dispositivo quindi durante l'unpairing non voglio fare il checkRemoveDevice per non aggiornare la UI due volte (una qui e una in on_remove_finished), ma solo in on_remove_finished quando ho la conferma che l'unpairing è andato a buon fine. Se invece il device viene rimosso da BlueZ per altri motivi (Es: dispositivo esce da portata, utente clicca "Annulla" durante il pairing, ecc) allora voglio fare il checkRemoveDevice per aggiornare la UI e mostrare che il device è stato rimosso/disconnesso.
        if(!unpairing_in_progress)
        {
            checkRemoveDevice(addr); 
        }

        for (int i = 0; i < device_count; i++)
        {
            if (strcmp(devices[i].address, addr) == 0)
            {
                DEBUG_PRINT("object-removed: device %s rimosso dal bus.\n", addr);

                // Rimuovo il device dall'array: shift a sinistra sovrascrivendo l'elemento da rimuovere, e decremento il contatore
                for (int j = i; j < device_count - 1; j++)
                {
                    devices[j] = devices[j + 1];
                }                
                device_count--;
                break;
            }
        }
        g_variant_unref(addr_var);
    }

    // Aggiorno la UI async-safe per rimuovere il device dalla lista
    createDeviceList(devices, device_count);

    g_object_unref(iface);
}

/* -------------------------------------------------------------------------
 * THREAD SCANSIONE
 * ------------------------------------------------------------------------- */

void load_paired_devices() 
{
    if (!bt_manager) return;    

    /* --- CHIEDIAMO AL MANAGER TUTTI GLI OGGETTI CHE BLUEZ GESTISCE AL MOMENTO (PAIRED) --- */
    GList *objects = g_dbus_object_manager_get_objects(bt_manager);
    GList *l;
    
    for (l = objects; l != NULL; l = l->next) 
    {
        // Uso la mia funzione di callback per processare i device paired in memoria, in modo da riempire l'array e aggiornare la UI
        GDBusObject *obj = G_DBUS_OBJECT(l->data);
        device_added_cb(bt_manager, obj, NULL);
    }

    // Pulisco la lista di oggetti restituita dal manager
    g_list_free_full(objects, g_object_unref);
    
    INFO_PRINT("load_paired_devices: caricate %d entry conosciute dal manager.\n", device_count);
}

void scanBthNet()
{
    /* --- 0. CHECK INIZIALIZZAZIONE --- */
    if (!bt_conn || !bt_adapter_obj) return;

    /* --- 1. RESET ARRAY DEVICES E PULIZIA UI --- */
    bth_clear_devices();
    createDeviceList(devices, device_count); // Aggiorna UI con lista vuota

    /* --- 2. CARICAMENTO E VISUALIZZAZIONE IMMEDIATA DEI DISPOSITIVI GIA PAIRED (SE CE NE SONO) --- */
    load_paired_devices();
    createDeviceList(devices, device_count); // Aggiorna UI con lista dei devices paired

    /* --- 3. GESTIONE RIAVVIO SCANSIONE SE GIA IN CORSO --- */
    if (scanning) 
    {
        stop_scan();
        g_usleep(30 * 1000); // 30ms per stabilità BlueZ
    }

    /* --- 4. PROTEZIONE HANDLER (EVITA DUPLICATI SUI SEGNALI) --- */
    if (device_added_handler_id == 0) 
    {
        device_added_handler_id = g_signal_connect(
                        bt_manager,
                        "object-added",
                        G_CALLBACK(device_added_cb), // Assicurati si chiami device_added_cb o bth_on_device_added
                        NULL
                    );
    }

    const gchar* adapter_path = g_dbus_object_get_object_path(bt_adapter_obj);
    GError* error = NULL;

    /* --- 5. CHIAMATA DBUS  --- */
    g_dbus_connection_call(
        bt_conn,
        "org.bluez",
        adapter_path,
        "org.bluez.Adapter1",
        "StartDiscovery",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        &error
    );

    if (error) {
        ERROR_PRINT("Errore StartDiscovery: %s\n", error->message);
        g_error_free(error);
        scanning = FALSE;
    } 
    else 
    {
        INFO_PRINT("StartDiscovery avviata.\n");
        ui_log_async("--- SCANSIONE BTH AVVIATA ---");
        scanning = TRUE;
    }
}

void stop_scan()
{
    if (!scanning)
    {
        ERROR_PRINT("Error: Nessuna scansione in corso\n");
        return;
    }

    // Controllo sempre che sia presente l'adapter e mi salvo il path se c'è
    if (!bt_adapter_obj)
        return;

    const gchar* adapter_path = g_dbus_object_get_object_path(bt_adapter_obj);

    // Comunico attraverso il DBus di effettuare la StopDiscovery
    g_dbus_connection_call(
        bt_conn,
        "org.bluez",
        adapter_path,
        "org.bluez.Adapter1",
        "StopDiscovery",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL
    );

    INFO_PRINT("Scan Bluetooth fermata.\n");

    // Pulisco variabili
    scanning = FALSE;  
}

/* -------------------------------------------------------------------------
 * INIT / DEINIT
 * ------------------------------------------------------------------------- */

void register_bth_agent(void) 
{
    if (bt_conn == NULL) { return; }

    GError *error = NULL;

    /* --- 1. PARSE DELL'XML DI INTROSPEZIONE --- */

    // Salviamo node_info per poterla liberare alla fine
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(agent_introspection_xml, &error);
    if (!node_info)
    {
        ERROR_PRINT("Errore parsing XML agent: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return;
    }

    /* --- 2. REGISTRIAMO L'OGGETTO AGENT SUL BUS --- */
    error = NULL;
    agent_reg_id = g_dbus_connection_register_object(
        bt_conn,
        "/test/agent",
        node_info->interfaces[0],
        &agent_vtable,
        NULL, NULL,
        &error
    );

    g_dbus_node_info_unref(node_info); /* liberiamo subito: non serve più */

    if (agent_reg_id == 0)
    {
        ERROR_PRINT("Errore registrazione oggetto Agent: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return;
    }

    /* --- 3. OTTENIAMO IL PROXY PER AgentManager1 --- */
    error = NULL;
    GDBusProxy *manager = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1",
        NULL,
        &error
    );

    if (!manager)
    {
        ERROR_PRINT("Errore creazione proxy AgentManager1: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        /* L'oggetto è già registrato sul bus: lo de-registriamo per pulizia */
        g_dbus_connection_unregister_object(bt_conn, agent_reg_id);
        agent_reg_id = 0;
        return;
    }

    /* --- 4. RegisterAgent --- */
    error = NULL;
    GVariant *reg_result = g_dbus_proxy_call_sync(
        manager,
        "RegisterAgent",
        g_variant_new("(os)", "/test/agent", "DisplayYesNo"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        &error
    );

    if (!reg_result)
    {
        ERROR_PRINT("Errore RegisterAgent: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
        g_object_unref(manager);
        g_dbus_connection_unregister_object(bt_conn, agent_reg_id);
        agent_reg_id = 0;
        return;
    }
    g_variant_unref(reg_result);

    /* --- 5. RequestDefaultAgent --- */
    error = NULL;
    GVariant *def_result = g_dbus_proxy_call_sync(
        manager,
        "RequestDefaultAgent",
        g_variant_new("(o)", "/test/agent"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        &error
    );

    if (!def_result)
    {
        /* Non fatale: l'agent è già registrato, semplicemente non è il default. Logghiamo il warning ma non annulliamo la registrazione. */
        ERROR_PRINT("Warning RequestDefaultAgent: %s\n", error ? error->message : "Unknown");
        if (error) g_error_free(error);
    }
    else
    {
        g_variant_unref(def_result);
    }

    g_object_unref(manager);

    INFO_PRINT("Bluetooth Agent registrato con successo.\n");
}

int logic_init_bth_mode()
{
    INFO_PRINT("--- INIT BTH MODE ---\n");

    GError* error = NULL;

    // Connessione al bus di sistema
    bt_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bt_conn) 
    {
        ERROR_PRINT("Errore connessione system bus: %s\n", error->message);
        g_error_free(error);
        return -1;
    }

    // Crea ObjectManager client
    bt_manager = g_dbus_object_manager_client_new_sync(bt_conn,
                                                       G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                       "org.bluez",
                                                       "/",
                                                       NULL, NULL, NULL,
                                                       NULL,
                                                       &error);

    if (!bt_manager) 
    {
        ERROR_PRINT("Errore creazione ObjectManager: %s\n", error->message);
        g_error_free(error);
        g_object_unref(bt_conn);
        bt_conn = NULL;
        return -1;
    }

    // Trova adapter
    if (!find_first_adapter()) 
    {
        ERROR_PRINT("Nessun adapter Bluetooth disponibile!\n");
        g_object_unref(bt_manager);
        bt_manager = NULL;
        g_object_unref(bt_conn);
        bt_conn = NULL;
        return -1;
    }

    // Registra agent per pairing
    if (bt_conn != NULL) {
        register_bth_agent();
        DEBUG_PRINT("Bluetooth Agent inizializzato.\n");
    }

    // Registra listener object-removed per rilevare quando BlueZ rimuove un device dal bus (es. dopo RemoveDevice/unpair): necessario perché in quel caso PropertiesChanged non viene emesso
    bt_device_removed_sig_id = g_signal_connect(
        bt_manager,
        "object-removed",
        G_CALLBACK(on_device_removed_cb),
        NULL
    );

    INFO_PRINT("Connessione Bluetooth inizializzata correttamente.\n");
    INFO_PRINT("--------------------------------------------------\n");    
    ui_log_async("--- MODULO BLUETOOTH INIZIALIZZATO ---");
    return 0;
}

void logic_deinit_bth_mode() 
{ 
    INFO_PRINT("--- DEINIT BTH MODE ---\n");
    INFO_PRINT("Deinizializzazione Bluetooth e Agent...\n");

    if (bt_conn != NULL) {
        // 1. Unregister Agent presso BlueZ
        GDBusProxy *manager = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, 
            NULL, "org.bluez", "/org/bluez", 
            "org.bluez.AgentManager1", NULL, NULL
        );

        if (manager) {
            g_dbus_proxy_call_sync(manager, "UnregisterAgent", 
                                  g_variant_new("(o)", "/test/agent"), 
                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_object_unref(manager);
        }

        // 2. Rimuoviamo l'oggetto D-Bus dalla nostra connessione
        if (agent_reg_id > 0) {
            g_dbus_connection_unregister_object(bt_conn, agent_reg_id);
            agent_reg_id = 0;
        }
    }

    DEBUG_PRINT("Bluetooth Agent deinizializzato.\n");

    // Se la scansione è in corso la fermo
    if(scanning)
    {
        stop_scan(); 
    }

    // Rimuovo e nel caso spengo il collegamento con l'adapter
    if (bt_adapter_obj) 
    { 
        if (!adapter_original_powered) 
        {   
            // Spegni adapter se lo abbiamo acceso noi 
            GError* error = NULL; GDBusProxy* adapter_proxy = g_dbus_proxy_new_sync( bt_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", g_dbus_object_get_object_path(bt_adapter_obj), "org.bluez.Adapter1", NULL, &error ); 
            if (adapter_proxy) 
            { 
                g_dbus_proxy_call_sync( adapter_proxy, "org.freedesktop.DBus.Properties.Set", g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(FALSE)), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error ); 
                if (error) 
                { 
                    ERROR_PRINT("Errore spegnimento adapter: %s\n", error->message); 
                    g_error_free(error); 
                } 
                else 
                { 
                    DEBUG_PRINT("Adapter spento.\n"); 
                } 
                g_object_unref(adapter_proxy); 
            } 
        } 
        g_object_unref(bt_adapter_obj); 
        bt_adapter_obj = NULL; 
    } 

    // Rimuovo handler collegato al segnale con un certo id
    if (device_added_handler_id != 0) 
    {
        g_signal_handler_disconnect(
                            bt_manager,
                            device_added_handler_id
                        );
        device_added_handler_id = 0;
    }

    // Disconnetto il listener object-removed
    if (bt_device_removed_sig_id != 0 && bt_manager != NULL)
    {
        g_signal_handler_disconnect(bt_manager, bt_device_removed_sig_id);
        bt_device_removed_sig_id = 0;
    }

    // Fermo il watch PropertiesChanged e il timer UI se un pairing era in corso
    bt_stop_pairing_timer();
    bt_stop_pairing_watch();

    // Chiudo manager
    if (bt_manager) 
    { 
        g_object_unref(bt_manager); 
        bt_manager = NULL; 
    } 

    // Chiudo connessione con DBus
    if (bt_conn) 
    { 
        g_object_unref(bt_conn); bt_conn = NULL; 
    }   

    // Pulisco array dispositivi trovati
    bth_clear_devices(); 

    INFO_PRINT("Connessione Bluetooth deinizializzata.\n"); 
    INFO_PRINT("--------------------------------------\n");
    ui_log_async("--- MODULO BLUETOOTH DEINIZIALIZZATO ---");
}

/* -------------------------------------------------------------------------
 * FUNZIONI PUBBLICHE PER LA GUI
 * ------------------------------------------------------------------------- */

void bth_clear_devices()
{
    device_count = 0;
}

int bth_get_device_count()
{
    return device_count;
}
