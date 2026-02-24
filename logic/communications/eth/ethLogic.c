#include "../../logic.h"
#include "../mainCommsLogic.h"
#include "ethLogic.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>    
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/

#define ETH_IFACE                "eth0"              // Nome interfaccia ethernet
#define ETH_LOCAL_IP             "172.16.100.10"     // Indirizzo ip interfaccia ethernet
#define ETH_SERVER_IP            "172.16.100.1"      // Indirizzo ip del server per iperf3

#define ETH_OPERSTATE_PATH       "/sys/class/net/" ETH_IFACE "/operstate"  // Percorso per leggere stato interfaccia
#define ETH_IFACE_CHECK_PERIOD   3                   // Secondi tra un check interfaccia e l'altro

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

static pthread_t eth_thread;                        // Handle del thread ETH
static atomic_int eth_thread_running = 0;           // Flag per fermare il thread
static atomic_int eth_run_test = 0;                 // Flag per triggerare il test iperf3
static atomic_int eth_thread_created = 0;           // Flag: il thread è stato creato con successo
static int prev_is_up = -1;                         // Variabile per controllare se cambio lo stato dell'interfaccio per non stamparne costantemente lo stato

/* 
=======================================
    PROTOTIPI FUNZIONI
=======================================
*/

/*
 * Thread principale per tutte le operazioni bloccanti ETH.
 * Loop:
 *   1. Controlla lo stato dell'interfaccia ogni ETH_IFACE_CHECK_PERIOD secondi.
 *   2. Se eth_run_test è settato, verifica che eth0 sia UP prima di eseguire
 *      il test iperf3, poi resetta il flag.
 *
 * Lo sleep è a step da 1 secondo per poter rispondere velocemente
 * al flag eth_thread_running = 0 senza bloccare il join per troppo tempo.
 */
static void *eth_thread_func(void *arg);
static int   eth_check_interface(void);       // Controlla se eth0 è up leggendo /sys/class/net/eth0/operstate
static void  eth_run_iperf3_test(void);       // Esegue iperf3 verso il server e stampa il risultato nella textArea

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* -------------------------------------------------------------------------
 * TEST IPERF3
 * ------------------------------------------------------------------------- */

static void eth_run_iperf3_test(void)
{
    ui_comms_log_async("--- [ETH] AVVIO TEST IPERF3 VERSO " ETH_SERVER_IP "... ---");
    INFO_PRINT("[ETH] Avvio iperf3 verso " ETH_SERVER_IP "\n");

    /* 1. Con popen eseguo il comando potendo leggerne l'output */
    FILE *pipe = popen("iperf3 -c " ETH_SERVER_IP " -t 5 2>&1", "r");
    if (!pipe)
    {
        ui_comms_log_async("[ETH] ERRORE: impossibile avviare iperf3");
        INFO_PRINT("[ETH] ERRORE: impossibile avviare iperf3\n");
        return;
    }

    /* 2. Stampo in log l'output del comando */
    char line[256];
    while (fgets(line, sizeof(line), pipe) != NULL)
    {
        // Rimuove newline finale prima di loggare
        line[strcspn(line, "\n")] = '\0';
        ui_comms_log_async("%s", line);
        INFO_PRINT("%s\n", line);
    }

    /* 3. Richiudo il file che avevo aperto */
    int ret = pclose(pipe);
    if (ret == 0)
    {
        ui_comms_log_async("--- [ETH] TEST IPERF3 COMPLETATO CON SUCCESSO ---");
        INFO_PRINT("[ETH] iperf3 completato con successo\n");
    }
    else
    {
        ui_comms_log_async("--- [ETH] TEST IPERF3 TERMINATO CON ERRORE (ret=%d) ---", ret);
        INFO_PRINT("[ETH] iperf3 terminato con errore (ret=%d)\n", ret);
    }
}

/* -------------------------------------------------------------------------
 * CHECK INTERFACCIA
 * ------------------------------------------------------------------------- */

static int eth_check_interface(void)
{
    /* Leggo il contenuto del file in ETH_OPERSTATE_PATH che contiene lo stato dell'interfaccia specificata */
    FILE *f = fopen(ETH_OPERSTATE_PATH, "r");
    if (!f)
    {
        ui_comms_log_async("[ETH] Impossibile aprire " ETH_OPERSTATE_PATH);
        return 0;
    }

    char state[32] = {0};
    fgets(state, sizeof(state), f);
    fclose(f);

    // Rimuove eventuale newline finale
    state[strcspn(state, "\n")] = '\0';

    return (strcmp(state, "up") == 0);
}

/* -------------------------------------------------------------------------
 * THREAD ETH
 * ------------------------------------------------------------------------- */

static void *eth_thread_func(void *arg)
{
    (void)arg;

    INFO_PRINT("[ETH] Thread avviato\n");

    int seconds_elapsed = ETH_IFACE_CHECK_PERIOD; // forza check immediato all'avvio

    while (atomic_load(&eth_thread_running))
    {
        /* ---- Check interfaccia periodico ---- */ 
        if (seconds_elapsed >= ETH_IFACE_CHECK_PERIOD)
        {
            seconds_elapsed = 0;
            int is_up = eth_check_interface();
            if(prev_is_up != is_up)
            {
                prev_is_up = is_up;
                if (is_up)
                {
                    ui_comms_log_async("[ETH] Interfaccia " ETH_IFACE ": UP");
                    INFO_PRINT("[ETH] Interfaccia " ETH_IFACE ": UP\n");
                }
                else
                {
                    ui_comms_log_async("[ETH] Interfaccia " ETH_IFACE ": DOWN");
                    INFO_PRINT("[ETH] Interfaccia " ETH_IFACE ": DOWN\n");
                }
            }            
        }


        /* Se devo fare il test controllo prima che l'interfaccia sia UP */
        if (atomic_load(&eth_run_test))
        {
            atomic_store(&eth_run_test, 0);

            if (eth_check_interface())
            {
                eth_run_iperf3_test();
            }
            else
            {
                ui_comms_log_async("[ETH] ERRORE: interfaccia " ETH_IFACE " DOWN, test iperf3 annullato");
                INFO_PRINT("[ETH] Test annullato: interfaccia " ETH_IFACE " DOWN\n");
            }
        }

        sleep(1);
        seconds_elapsed++;
    }

    INFO_PRINT("[ETH] Thread terminato\n");

    return NULL;
}

/* -------------------------------------------------------------------------
 * RICHIEDE INVIO MESSAGGIO DI TEST
 * ------------------------------------------------------------------------- */

void sendTestMessageEth(void)
{
    if(atomic_load(&eth_run_test) == 0)
    {
        INFO_PRINT("[ETH] Test richiesto\n");
        ui_comms_log_async("--- [ETH] TEST IPERF3 ---");
        atomic_store(&eth_run_test, 1);
    }
    else
    {
        INFO_PRINT("[ETH] Test già in corso, attendere\n");
        ui_comms_log_async("Test già in corso attendere...");
    }
}

/* -------------------------------------------------------------------------
 * INIT / DEINIT
 * ------------------------------------------------------------------------- */

int logic_init_eth_mode(void)
{
    atomic_store(&eth_run_test,       0);
    atomic_store(&eth_thread_created, 0);
    atomic_store(&eth_thread_running, 1);

    if (pthread_create(&eth_thread, NULL, eth_thread_func, NULL) != 0)
    {
        atomic_store(&eth_thread_running, 0);
        INFO_PRINT("[ETH] ERRORE: impossibile creare il thread\n");
        ui_comms_log_async("[ETH] ERRORE: impossibile creare il thread");
        return -1;
    }

    /* Il thread è stato creato con successo: da ora deinit può fare join */
    atomic_store(&eth_thread_created, 1);

    INFO_PRINT("--- ETH MODE INIZIALIZZATO ---\n");
    ui_comms_log_async("--- ETH MODE INIZIALIZZATO ---");

    return 0;
}

void logic_deinit_eth_mode(void)
{
    atomic_store(&eth_run_test,       0);
    atomic_store(&eth_thread_running, 0);

    if (atomic_load(&eth_thread_created))
    {
        pthread_join(eth_thread, NULL);
        atomic_store(&eth_thread_created, 0);
    }

    INFO_PRINT("--- ETH MODE DEINIZIALIZZATO ---\n");
    ui_comms_log_async("--- ETH MODE DEINIZIALIZZATO ---");
}