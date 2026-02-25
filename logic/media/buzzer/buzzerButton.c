#define _POSIX_C_SOURCE 199309L

#include "../../logic.h"
#include "buzzerButton.h"
#include "buzzerPwm.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>

/*
=====================================
    DEFINIZIONI
=====================================
*/

typedef enum {
    BTN_ACTION_TONE_UP,
    BTN_ACTION_TONE_DOWN
} BtnAction;

/*
=====================================
    VARIABILI STATICHE
=====================================
*/

static pthread_t    btn_thread;
static atomic_bool  thread_running = false;
static int          fd_input = -1;

/*
=====================================
    PROTOTIPI
=====================================
*/

static int64_t now_ms(void);                        // @brief  Restituisce il timestamp corrente in millisecondi (CLOCK_MONOTONIC).
static void btn_action_cb(void *user_data);         // @brief  Callback eseguita nel contesto LVGL: applica l'azione tone_up/tone_down al PWM. @param  user_data  Puntatore a BtnAction allocato dal thread, liberato qui.
static void *btn_thread_fn(void *arg);              // @brief  Corpo del thread di lettura: usa poll() con timeout per attendere eventi da /dev/input, filtra EV_KEY key-down e applica debounce software.
static int input_setup(void);                       // @brief  Apre il device /dev/input in modalità O_NONBLOCK. @return  0 successo | -1 errore
static void input_cleanup(void);                    // @brief  Chiude il file descriptor del device di input.

/*
=====================================
    IMPLEMENTAZIONI
=====================================
*/

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void btn_action_cb(void *user_data)
{
    BtnAction *action = (BtnAction *)user_data;

    /* Controllo che il thread sia ancora in esecuzione */
    if (!atomic_load(&thread_running))
    {
        free(action);
        return;
    }

    /* Controllo che il buzzer sia acceso */
    if (!buzzer_logic_is_on())
    {
        free(action);
        return;
    }

    switch (*action)
    {
        case BTN_ACTION_TONE_UP:
        {
            int ret = buzzer_pwm_tone_up();
            if (ret == 1)
                INFO_PRINT("Tono già al massimo\n");
            else if (ret != 0)
                ERROR_PRINT("Errore nel cambio tono (up)\n");
            break;
        }
        case BTN_ACTION_TONE_DOWN:
        {
            int ret = buzzer_pwm_tone_down();
            if (ret == 1)
                INFO_PRINT("Tono già al minimo\n");
            else if (ret != 0)
                ERROR_PRINT("Errore nel cambio tono (down)\n");
            break;
        }
        default: break;
    }

    free(action);
}

static void *btn_thread_fn(void *arg)
{
    (void)arg;

    int64_t last_btn1_ms = 0;
    int64_t last_btn2_ms = 0;

    // Struttura per poll(): monitoriamo fd_input in attesa di dati disponibili in lettura (POLLIN)
    struct pollfd pfd = {
        .fd     = fd_input,
        .events = POLLIN
    };

    while (atomic_load(&thread_running))
    {
        // Attende fino a 100ms un evento sul device di input. In questo modo non ci blocchiamo nel thread in attesa di un evento
        int ret = poll(&pfd, 1, 100);

        if (ret < 0)
        {
            ERROR_PRINT("Errore poll() su input device\n");
            break;
        }

        if (ret == 0)
            continue;   // timeout scaduto, nessun evento: ricontrolliamo thread_running

        // poll() ha segnalato dati disponibili: leggiamo tutti gli eventi in coda.
        struct input_event ev;
        while (read(fd_input, &ev, sizeof(ev)) == sizeof(ev))
        {
            // Filtriamo: ci interessano solo EV_KEY con value 1 (key down).
            if (ev.type != EV_KEY || ev.value != 1)
                continue;

            int64_t now = now_ms();

            if (ev.code == KEY_1)
            {
                // Debounce software: scartiamo pressioni troppo ravvicinate. Ne esiste già uno hardware
                if (now - last_btn1_ms >= BTN_DEBOUNCE_MS)
                {
                    last_btn1_ms = now;
                    INFO_PRINT("BTN1 premuto — tone up\n");

                    // Allochiamo l'azione e la passiamo a LVGL tramite lv_async_call per essere thread-safe
                    BtnAction *action = malloc(sizeof(BtnAction));
                    if (action)
                    {
                        *action = BTN_ACTION_TONE_UP;
                        lv_async_call(btn_action_cb, action);
                    }
                }
            }
            else if (ev.code == KEY_2)
            {
                if (now - last_btn2_ms >= BTN_DEBOUNCE_MS)
                {
                    last_btn2_ms = now;
                    INFO_PRINT("BTN2 premuto — tone down\n");

                    BtnAction *action = malloc(sizeof(BtnAction));
                    if (action)
                    {
                        *action = BTN_ACTION_TONE_DOWN;
                        lv_async_call(btn_action_cb, action);
                    }
                }
            }
        }
    }

    DEBUG_PRINT("Thread bottoni terminato\n");
    return NULL;
}

static int input_setup(void)
{
    fd_input = open(INPUT_DEVICE_PATH, O_RDONLY | O_NONBLOCK);
    if (fd_input < 0)
    {
        perror(INPUT_DEVICE_PATH);
        return -1;
    }
    return 0;
}

static void input_cleanup(void)
{
    if (fd_input >= 0)
    {
        close(fd_input);
        fd_input = -1;
    }
}

/*
=====================================
    FUNZIONI PUBBLICHE
=====================================
*/

int buzzer_buttons_start(void)
{
    if (input_setup() != 0)
        return -1;

    atomic_store(&thread_running, true);

    if (pthread_create(&btn_thread, NULL, btn_thread_fn, NULL) != 0)
    {
        ERROR_PRINT("pthread_create btn_thread failed");
        atomic_store(&thread_running, false);
        input_cleanup();
        return -1;
    }

    DEBUG_PRINT("Thread bottoni avviato\n");
    return 0;
}

void buzzer_buttons_stop(void)
{
    atomic_store(&thread_running, false);
    pthread_join(btn_thread, NULL);
    input_cleanup();
    DEBUG_PRINT("Thread bottoni fermato\n");
}