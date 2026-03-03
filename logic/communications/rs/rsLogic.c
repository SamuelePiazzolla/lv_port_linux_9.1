#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "../../logic.h"
#include "../mainCommsLogic.h"
#include "rsLogic.h"

/* 
=======================================
    DEFINIZIONI
=======================================
*/

// Comando e argomento del processo slave RS-485
#define RS_SLAVE_CMD   "./lts00822_dut_sw"
#define RS_SLAVE_ARG1  "slave"
#define RS_SLAVE_ARG2  "/dev/ttySC1"

// Intervallo di polling della pipe in millisecondi. 
#define RS_POLL_PERIOD_MS   100

// Dimensione del buffer di lettura dalla pipe per singolo tick del timer. 
#define RS_READ_BUF_SIZE    256

// Timeout in multipli di 10ms prima di scalare da SIGINT a SIGKILL. ( 100 × 10ms = 1s )
#define RS_SIGINT_TIMEOUT_TICKS 100

/* 
=======================================
    VARIABILI GLOBALI STATICHE
=======================================
*/

static pid_t slave_pid = -1;                // PID del processo slave. Valore sentinella -1 = nessun processo attivo.  
static int slave_pipe_fd = -1;              // File descriptor del lato lettura della pipe (stdout/stderr dello slave). -1 = pipe non aperta.
static lv_timer_t *rs_poll_timer = NULL;    // Riferimento al lv_timer di polling. Necessario per poterlo eliminare quando lo slave termina o viene stoppato

/* 
=======================================
    PROTOTIPI FUNZIONI PRIVATE
=======================================
*/

/* -------------------------------------------------------------------------

 * Callback del lv_timer, eseguita ogni RS_POLL_PERIOD_MS ms nel main loop
 * LVGL.
 *
 * Responsabilità:
 *   1. Legge tutto l'output disponibile dalla pipe in modo non bloccante
 *   2. Appende ogni riga alla textarea.
 *   3. Controlla se lo slave è terminato spontaneamente, e in quel caso fa cleanup.
 * 
 * ------------------------------------------------------------------------- */
static void rs_poll_timer_cb(lv_timer_t *timer);
static void rs_stop_slave(void);                    // Termina il processo slave in modo pulito
static void rs_cleanup_resources(void);             // Rilascia tutte le risorse associate all slave (File descriptor e timer di poll)

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* -------------------------------------------------------------------------
 * GESTIONE SLAVE
 * ------------------------------------------------------------------------- */

static void rs_cleanup_resources(void)
{
    if (slave_pipe_fd != -1) {
        close(slave_pipe_fd);
        slave_pipe_fd = -1;
    }

    if (rs_poll_timer != NULL) {
        lv_timer_del(rs_poll_timer);
        rs_poll_timer = NULL;
    }

    slave_pid = -1;
}

static void rs_stop_slave(void)
{
    if (slave_pid == -1)
        return;

    INFO_PRINT("rs_stop_slave: invio SIGINT a PID %d\n", slave_pid);
    ui_comms_log_async("[RS-485] Invio SIGINT allo slave (PID %d)...", slave_pid);

    kill(slave_pid, SIGINT);

    // Polling non bloccante: aspettiamo che lo slave termini dopo SIGINT.
    for (int i = 0; i < RS_SIGINT_TIMEOUT_TICKS; i++) 
    {
        usleep(10 * 1000);     // usleep(10ms) × RS_SIGINT_TIMEOUT_TICKS = 1s di timeout totale.
        int status;
        if (waitpid(slave_pid, &status, WNOHANG) == slave_pid) 
        {
            INFO_PRINT("rs_stop_slave: slave terminato pulitamente\n");
            ui_comms_log_async("[RS-485] SLAVE TERMINATO");
            rs_cleanup_resources();
            return;
        }
    }

    // Timeout scaduto: il processo non ha risposto a SIGINT → SIGKILL.
    ERROR_PRINT("rs_stop_slave: timeout SIGINT, invio SIGKILL a PID %d\n", slave_pid);
    ui_comms_log_async("[RS-485] Timeout: invio SIGKILL allo slave");

    kill(slave_pid, SIGKILL);
    // Aspettiamo in maniera bloccante tanto è istantanea
    waitpid(slave_pid, NULL, 0);


    // Pulisco le risorse occupate dallo slave
    rs_cleanup_resources();
}

static void rs_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (slave_pipe_fd == -1)
        return;

    char buf[RS_READ_BUF_SIZE];
    ssize_t n;

    // Loop di lettura: consumiamo tutto l'output disponibile in questo tick.
    while ((n = read(slave_pipe_fd, buf, sizeof(buf) - 1)) > 0) 
    {
        buf[n] = '\0';

        // Rimuoviamo il '\n' finale se presente: ui_comms_log_async lo aggiunge già
        if (n > 0 && buf[n - 1] == '\n')
            buf[n - 1] = '\0';

        // Aggiornamento asincrono alla textarea (non necessario il fatto che sia asincrono)
        ui_comms_log_async("%s", buf);
    }

    // Verifichiamo se lo slave è terminato spontaneamente.
    int status;
    if (waitpid(slave_pid, &status, WNOHANG) == slave_pid) 
    {
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        INFO_PRINT("rs_poll_timer_cb: slave terminato spontaneamente (exit code %d)\n", exit_code);
        ui_comms_log_async("[RS-485] Slave terminato (exit code %d)", exit_code);
        rs_cleanup_resources();
    }
}

/* -------------------------------------------------------------------------
 * START TEST
 * ------------------------------------------------------------------------- */

void sendTestMessageRs(void)
{
    // Se lo slave è già attivo lo stoppiamo prima di avviarne uno nuovo.
    if (slave_pid != -1) 
    {
        INFO_PRINT("--- RS-485 TEST PING PONG FERMATO E RICOMINCIATO  ---\n");
        ui_comms_log_async("--- RS-485 TEST PING PONG FERMATO E RICOMINCIATO ---\n");
        rs_stop_slave();
    }
    else
    {
        INFO_PRINT("--- RS-485 TEST PING PONG COMINCIATO  ---\n");
        ui_comms_log_async("--- RS-485 TEST PING PONG COMINCIATO ---\n");

    }

    INFO_PRINT("sendTestMessageRs: avvio %s %s %s\n", RS_SLAVE_CMD, RS_SLAVE_ARG1, RS_SLAVE_ARG2);

    // Creiamo la pipe: pipefd[0] = lettura (padre), pipefd[1] = scrittura (figlio).
    int pipefd[2];
    if (pipe(pipefd) == -1) 
    {
        ERROR_PRINT("sendTestMessageRs: pipe() fallita: %s\n", strerror(errno));
        ui_comms_log_async("[RS-485] ERRORE: impossibile creare la pipe (%s)", strerror(errno));
        return;
    }

    pid_t pid = fork();

    if (pid < 0) 
    {
        ERROR_PRINT("sendTestMessageRs: fork() fallita: %s\n", strerror(errno));
        ui_comms_log_async("[RS-485] ERRORE: fork fallita (%s)", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) 
    {
        /* ----------------------------------------------------------------
         * PROCESSO FIGLIO
         * ----------------------------------------------------------------
         * Il figlio non legge dalla pipe --> chiude pipefd[0].
         * Redirige stdout e stderr su pipefd[1] così tutto l'output
         * dello slave finisce nel canale letto dal padre.
         * Usiamo _exit() e non exit() per non flushare i buffer stdio
         * del processo padre (comportamento indefinito dopo fork).
         * ---------------------------------------------------------------- */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execl(RS_SLAVE_CMD, RS_SLAVE_CMD, RS_SLAVE_ARG1, RS_SLAVE_ARG2, NULL);

        // Se execl ritorna, c'è stato un errore.
        ERROR_PRINT("[RS-485] execl fallita: %s\n", strerror(errno));
        ui_comms_log_async("[RS-485] execl fallita: %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }

    /* --------------------------------------------------------------------
     * PROCESSO PADRE
     * --------------------------------------------------------------------
     * Chiudiamo pipefd[1] nel padre. IMPORTANTE: se non lo chiudiamo,
     * la pipe non raggiunge mai EOF anche quando lo slave termina,
     * perché il padre stesso mantiene aperto il lato scrittura.
     * -------------------------------------------------------------------- */
    close(pipefd[1]);

    // Impostiamo O_NONBLOCK sul lato lettura: quando la pipe è vuota,
    // read() ritorna EAGAIN invece di bloccarsi, preservando il main loop.
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags == -1 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) 
    {
        ERROR_PRINT("sendTestMessageRs: fcntl O_NONBLOCK fallito: %s\n", strerror(errno));
    }

    slave_pid = pid;
    slave_pipe_fd = pipefd[0];

    INFO_PRINT("sendTestMessageRs: slave avviato con PID %d\n", slave_pid);
    ui_comms_log_async("[RS-485] TEST avviato: %s %s %s", RS_SLAVE_CMD, RS_SLAVE_ARG1, RS_SLAVE_ARG2);

    // Creiamo il lv_timer di polling.
    rs_poll_timer = lv_timer_create(rs_poll_timer_cb, RS_POLL_PERIOD_MS, NULL);
    if (rs_poll_timer == NULL) {
        ERROR_PRINT("sendTestMessageRs: lv_timer_create fallita\n");
        ui_comms_log_async("[RS-485] ERRORE: impossibile creare il timer di polling");
        rs_stop_slave();
    }
}

/* -------------------------------------------------------------------------
 * INIT / DEINIT
 * ------------------------------------------------------------------------- */

int logic_init_rs_mode(void)
{
    INFO_PRINT("--- RS-485 MODE INIZIALIZZATO ---\n");

    // Pulizia difensiva delle variabili di stato: garantisce uno stato coerente anche se la deinit precedente non è stata chiamata.
    slave_pid     = -1;
    slave_pipe_fd = -1;
    rs_poll_timer = NULL;

    ui_comms_log_async("--- RS-485 MODE INIZIALIZZATO ---");
    return 0;
}

void logic_deinit_rs_mode(void)
{
    INFO_PRINT("--- RS-485 MODE DEINIZIALIZZATO ---\n");

    // Ferma lo slave se esiste
    if (slave_pid != -1) {
        rs_stop_slave();
    }

    ui_comms_log_async("--- RS-485 MODE DEINIZIALIZZATO ---");
}