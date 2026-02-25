#define _POSIX_C_SOURCE 199309L

#include "../../logic.h"
#include "buzzerButton.h"
#include "buzzerPwm.h"

#include <gpiod.h>
#include <pthread.h>
#include <stdatomic.h>      
#include <time.h>           

/*
=====================================
    DEFINIZIONI
=====================================
*/

// Identifica quale azione eseguire nella callback LVGL
typedef enum {
    BTN_ACTION_TONE_UP,
    BTN_ACTION_TONE_DOWN
} BtnAction;

/*
=====================================
    VARIABILI STATICHE
=====================================
*/

static pthread_t gpio_thread;                   // Thread gpio per gestione pulsanti fisici
static atomic_bool thread_running = false;      // flag di controllo thread e validità callback (C11 atomic)

static struct gpiod_chip *chip    = NULL;
static struct gpiod_line *line1   = NULL;       // usr_btn_1 — tone up
static struct gpiod_line *line2   = NULL;       // usr_btn_2 — tone down

/*
=====================================
    PROTOTIPI
=====================================
*/

static int64_t now_ms(void);                            // @brief  Restituisce il timestamp corrente in millisecondi.
static void lvgl_btn_action_cb(void *user_data);        // @brief  Callback di modifica in cui chiamiamo il modulo PWM per modificare il tono @param  user_data  Puntatore a BtnAction (allocato nel thread GPIO, liberato qui)
static void *gpio_thread_fn(void *arg);                 // @brief  Corpo del thread GPIO. Usa gpiod_line_event_wait() per attendere l'evento sui pulsanti fisici.
static int gpio_setup(void);                            // @brief  Apre il chip GPIO e richiede le due linee in modalità evento (falling edge, active-low). @return  0 successo | -1 errore
static void gpio_cleanup(void);                         // @brief  Rilascia le linee GPIO e chiude il chip.

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

static void lvgl_btn_action_cb(void *user_data)
{
    BtnAction *action = (BtnAction *)user_data;

    // Controllo se posso eseguire il thread
    if (!atomic_load(&thread_running))
    {
        free(action);
        return;
    }

    // Controlla se il buzzer è attivo: se è spento ignora
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

static void *gpio_thread_fn(void *arg)
{
    (void)arg;

    // Timestamp ultimo evento valido per ogni tasto (debounce)
    int64_t last_btn1_ms = 0;
    int64_t last_btn2_ms = 0;

    // Timeout di wait: 100ms. Non saturo CPU
    struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100 * 1000000L };

    while (atomic_load(&thread_running))
    {
        // --- Controllo btn1 ---
        int ret1 = gpiod_line_event_wait(line1, &timeout);
        if (ret1 < 0)
        {
            ERROR_PRINT("Errore attesa evento GPIO btn1\n");
            break;
        }

        bool btn1_acted = false;
        if (ret1 == 1)  // evento disponibile
        {
            struct gpiod_line_event ev1;
            if (gpiod_line_event_read(line1, &ev1) == 0 && ev1.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
            {
                int64_t now = now_ms();
                if (now - last_btn1_ms >= BTN_DEBOUNCE_MS)
                {
                    last_btn1_ms = now;
                    btn1_acted = true;  // btn1 ha prodotto un'azione reale: btn2 verrà saltato
                    INFO_PRINT("BTN1 premuto — tone up\n");

                    BtnAction *action = malloc(sizeof(BtnAction));
                    if (action)
                    {
                        *action = BTN_ACTION_TONE_UP;
                        lv_async_call(lvgl_btn_action_cb, action);
                    }
                }
            }
        }

        // --- Controllo btn2: saltato se btn1 ha già prodotto un'azione in questa iterazione ---
        // L'eventuale evento di btn2 rimane in coda kernel e viene processato al ciclo successivo
        if (btn1_acted) continue;

        int ret2 = gpiod_line_event_wait(line2, &timeout);
        if (ret2 < 0)
        {
            ERROR_PRINT("Errore attesa evento GPIO btn2\n");
            break;
        }
        if (ret2 == 1)
        {
            struct gpiod_line_event ev2;
            if (gpiod_line_event_read(line2, &ev2) == 0 && ev2.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
            {
                int64_t now = now_ms();
                if (now - last_btn2_ms >= BTN_DEBOUNCE_MS)
                {
                    last_btn2_ms = now;
                    INFO_PRINT("BTN2 premuto — tone down\n");

                    BtnAction *action = malloc(sizeof(BtnAction));
                    if (action)
                    {
                        *action = BTN_ACTION_TONE_DOWN;
                        lv_async_call(lvgl_btn_action_cb, action);
                    }
                }
            }
        }
    }

    DEBUG_PRINT("Thread GPIO terminato\n");
    return NULL;
}

static int gpio_setup(void)
{
    chip = gpiod_chip_open_by_name(GPIO_CHIP_NAME);
    if (!chip)
    {
        ERROR_PRINT("gpiod_chip_open_by_name");
        return -1;
    }

    line1 = gpiod_chip_get_line(chip, GPIO_LINE_BTN1);
    line2 = gpiod_chip_get_line(chip, GPIO_LINE_BTN2);
    if (!line1 || !line2)
    {
        ERROR_PRINT("Impossibile ottenere le linee GPIO\n");
        gpiod_chip_close(chip);
        chip = NULL;
        return -1;
    }

    // Richiede entrambe le linee per la ricezione di eventi falling edge
    if (gpiod_line_request_falling_edge_events_flags(line1, "buzzer_btn1", GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW) != 0)
    {
        ERROR_PRINT("gpiod_line_request btn1");
        gpiod_chip_close(chip);
        chip = NULL;
        return -1;
    }

    if (gpiod_line_request_falling_edge_events_flags(line2, "buzzer_btn2", GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW) != 0)
    {
        perror("gpiod_line_request btn2");
        gpiod_line_release(line1);
        gpiod_chip_close(chip);
        chip = NULL;
        return -1;
    }

    return 0;
}

static void gpio_cleanup(void)
{
    if (line1) { gpiod_line_release(line1); line1 = NULL; }
    if (line2) { gpiod_line_release(line2); line2 = NULL; }
    if (chip)  { gpiod_chip_close(chip);    chip  = NULL; }
}

/*
=====================================
    FUNZIONI PUBBLICHE
=====================================
*/

int buzzer_buttons_start(void)
{
    if (gpio_setup() != 0)
        return -1;

    // 1. Permetto al thread di eseguire le sue funzioni
    atomic_store(&thread_running, true);

    // 2. Creo il thread
    if (pthread_create(&gpio_thread, NULL, gpio_thread_fn, NULL) != 0)
    {
        ERROR_PRINT("pthread_create gpio_thread");
        atomic_store(&thread_running, false);
        gpio_cleanup();
        return -1;
    }

    DEBUG_PRINT("Thread GPIO bottoni avviato\n");
    return 0;
}

void buzzer_buttons_stop(void)
{
    // 1. Disattiva il thread, le eventuali callback in coda si scarteranno da sole
    atomic_store(&thread_running, false);

    // 2. Attende che il thread termini correttamente
    pthread_join(gpio_thread, NULL);

    // 3. Pulisco
    gpio_cleanup();
    DEBUG_PRINT("Thread GPIO bottoni fermato\n");
}