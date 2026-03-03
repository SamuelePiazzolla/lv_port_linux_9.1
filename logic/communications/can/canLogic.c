#include <time.h>
#include <stdatomic.h>

#include "vehicle_types.h"
#include "canLogic.h"
#include "can.h"
#include "display.h"


/* =======================================
    VARIABILI
   ======================================= */


static my_struct_t s_canData;           /* Struttura condivisa con can_rx_handler: contiene socket CAN + vehicle_t. */
static displayParam_t s_display;        /* Struttura display: inizializzata da display_init(), azzerata da display_deinit(). */
static pthread_t s_canRxThread;         /* Handle del thread di ricezione CAN. */
static pthread_t s_canLoopThread;       /* Handle del thread del loop principale. */
static atomic_bool s_stopLoop = false;  /* Flag atomico di stop per il thread loop. */
static bool s_initialized = false;      /* Indica se il modulo è stato inizializzato correttamente. */

/* =======================================
    PROTOTIPI
   ======================================= */

/*
 * Legge i dati veicolo e chiama update_display()
 *
 * Il loop è limitato a ~60 Hz con usleep(16000) per evitare di
 * saturare la coda interna di lv_async_call e sprecare CPU.
 */
static void *can_main_loop(void *arg);
static void loop_mutex_cleanup(void *arg); //Cleanup handler per thread can_main_loop

/* =======================================
    IMPLEMENTAZIONI
   ======================================= */
/* ---------------------------------------
    CLEANUP HANDLERS
   --------------------------------------- */

static void loop_mutex_cleanup(void *arg)
{
    pthread_mutex_t *mtx = (pthread_mutex_t *)arg;
    pthread_mutex_unlock(mtx);
}

/* ---------------------------------------
    THREAD LOOP PRINCIPALE
   --------------------------------------- */

static void *can_main_loop(void *arg)
{
    (void)arg;
    
    /* Abilita la cancellazione asincrona del thread. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    /* Buffer per memorizzare l'ultimo stato noto del veicolo */
    vehicle_t old_vehicle_struct;
    memset(&old_vehicle_struct, 0, sizeof(vehicle_t));

    while (!atomic_load(&s_stopLoop))
    {
        vehicle_t current_vehicle_struct;

        /* Registra il cleanup handler per il mutex PRIMA di acquisirlo. */
        pthread_cleanup_push(loop_mutex_cleanup, &s_canData.lock);

        /* 1. ACQUISISCI IL MUTEX: copia veloce dei dati e rilascio immediato per non tener bloccato il thread*/
        pthread_mutex_lock(&s_canData.lock);
        memcpy(&current_vehicle_struct, &s_canData.vehicle_struct, sizeof(vehicle_t));
        pthread_mutex_unlock(&s_canData.lock);

        /* Il mutex è già stato rilasciato sopra manualmente, quindi il cleanup handler deve essere deregistrato senza eseguirlo.
         * Se invece il thread viene cancellato TRA lock e unlock, pthread lo chiamerebbe automaticamente. */
        pthread_cleanup_pop(0);

        /* 2. VERIFICA LE DIFFERENZE: aggiorno solo se ci sono modifiche da fare*/
        if (memcmp(&current_vehicle_struct, &old_vehicle_struct, sizeof(vehicle_t)) != 0)
        {
            memcpy(&old_vehicle_struct, &current_vehicle_struct, sizeof(vehicle_t));
            update_display(&current_vehicle_struct, &s_display);
        }

        /* ~60 Hz */
        usleep(16000);
    }

    DEBUG_PRINT("[CAN LOGIC] Thread loop principale terminato\n");
    return NULL;
}
/* ---------------------------------------
    INIT / DEINIT
   --------------------------------------- */

void logic_init_can_mode(void)
{
    if (s_initialized)
    {
        DEBUG_PRINT("[CAN LOGIC] Attenzione: modulo già inizializzato\n");
        return;
    }

    INFO_PRINT("--------------------------------\n");

    /* 1. Reset stato interno */
    memset(&s_canData, 0, sizeof(my_struct_t));
    memset(&s_display, 0, sizeof(displayParam_t));
    s_canData.canSoket = -1;
    atomic_store(&s_stopLoop, false);

    /* 2. Inizializza il mutex prima di creare i thread */
    pthread_mutex_init(&s_canData.lock, NULL);

    /* 3. Inizializzo display e strutture dati */
    display_init(&s_display);

    /* 4. Apertura socket CAN */
    if (can_protocol_init(&s_canData.canSoket) != 0)
    {
        ERROR_PRINT("[CAN LOGIC] Errore: can_protocol_init fallito\n");
        pthread_mutex_destroy(&s_canData.lock);  /* cleanup mutex */
        return;
    }

    /* 5. Avvio thread lettura (RX) CAN */
    if (pthread_create(&s_canRxThread, NULL, can_rx_handler, (void *)&s_canData) != 0)
    {
        ERROR_PRINT("[CAN LOGIC] Errore creazione thread RX");
        close(s_canData.canSoket);
        s_canData.canSoket = -1;
        pthread_mutex_destroy(&s_canData.lock);  /* cleanup mutex */ 
        return;
    }

    /* 6. Avvio thread loop principale */
    if (pthread_create(&s_canLoopThread, NULL, can_main_loop, NULL) != 0)
    {
        ERROR_PRINT("[CAN LOGIC] Errore creazione thread loop");
        atomic_store(&s_stopLoop, 1);
        close(s_canData.canSoket);
        s_canData.canSoket = -1;
        pthread_cancel(s_canRxThread);           
        pthread_join(s_canRxThread, NULL);
        pthread_mutex_destroy(&s_canData.lock);  /* cleanup mutex */
        return;
    }

    s_initialized = true;
    INFO_PRINT("--- MODULO CAN INIZIALIZZATO ---\n");
    INFO_PRINT("--------------------------------\n");
    return;
}

void logic_deinit_can_mode(void)
{
    if (!s_initialized)
    {
        DEBUG_PRINT("[CAN LOGIC] Attenzione: deinit chiamata senza init\n");
        return;
    }

    INFO_PRINT("----------------------------------\n");

    /* 1. Ferma il thread loop:
    *    - Il flag atomico dice al loop di uscire al prossimo ciclo.
    *    - pthread_cancel interrompe immediatamente l'eventuale usleep()
    *      o pthread_mutex_lock() in corso, senza aspettare 16ms.
    *    I due meccanismi sono complementari e non si escludono. */
    atomic_store(&s_stopLoop, true);
    pthread_cancel(s_canLoopThread);

    /* 2. Ferma il thread RX:
     *    - close() non è sufficiente a sbloccare read() su
     *      socket CAN raw su Linux
     *    - pthread_cancel interrompe read() immediatamente perché
     *      read() è un cancellation point POSIX garantito. */
    pthread_cancel(s_canRxThread);
    if (s_canData.canSoket >= 0)
    {
        close(s_canData.canSoket);
        s_canData.canSoket = -1;
    }

    /* 3. Attende la terminazione pulita di entrambi i thread.*/
    DEBUG_PRINT("Chiudiamo i due thread\n");
    pthread_join(s_canRxThread,   NULL);
    DEBUG_PRINT("Thread lettura da can chiuso\n");
    pthread_join(s_canLoopThread, NULL);
    DEBUG_PRINT("Main loop chiuso\n");

    /* 4. Distruzione mutex dopo che i thread sono terminati */
    pthread_mutex_destroy(&s_canData.lock);

    /* 5. Azzera i puntatori LVGL PRIMA della distruzione della schermata.*/
    display_deinit(&s_display);

    /* 6. Reset completo dello stato interno */
    memset(&s_canData, 0, sizeof(my_struct_t));
    s_initialized = false;

    INFO_PRINT("--- MODULO CAN DEINIZIALIZZATO ---\n");
    INFO_PRINT("----------------------------------\n");
}