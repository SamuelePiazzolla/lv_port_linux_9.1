#include "buzzerPwm.h"
#include "../../logic.h"


#include <fcntl.h>      // open()
#include <errno.h>      // errno



/*
=====================================
    VARIABILI STATICHE
=====================================
*/

// Period corrente in nanosecondi (aggiornato ad ogni cambio tono)
static uint32_t current_period_ns = PWM_DEFAULT_PERIOD_NS;

/*
=====================================
    FUNZIONI PRIVATE
=====================================
*/

/**
 * @brief  Scrive una stringa in un file sysfs.
 *         Apre, scrive e chiude il file descriptor ad ogni chiamata
 *         (richiesto dal kernel per i file sysfs).
 * @param  path   Percorso assoluto del file sysfs
 * @param  value  Stringa da scrivere
 * @return  0 successo | -1 errore
 */
static int sysfs_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        // Uso perror per stampare il messaggio di errore del kernel
        perror(path);
        return -1;
    }

    ssize_t written = write(fd, value, strlen(value));
    close(fd);

    if (written < 0)
    {
        perror(path);
        return -1;
    }

    return 0;
}

/**
 * @brief  Aggiorna period e duty_cycle sul canale PWM.
 *         Il duty_cycle viene sempre impostato al 50% del period.
 *         ATTENZIONE: quando si abbassa il period (tono più alto) bisogna
 *         prima aggiornare il duty_cycle e poi il period, altrimenti il kernel
 *         può rifiutare la scrittura (duty_cycle > period).
 *         Viceversa quando si alza il period (tono più basso) prima il period.
 * @param  new_period_ns  Nuovo period in nanosecondi
 * @return  0 successo | -1 errore
 */
static int pwm_set_period(uint32_t new_period_ns)
{
    char buf_period[32];
    char buf_duty[32];

    uint32_t new_duty_ns = new_period_ns / 2;   // 50% duty cycle

    snprintf(buf_period, sizeof(buf_period), "%u", new_period_ns);
    snprintf(buf_duty,   sizeof(buf_duty),   "%u", new_duty_ns);

    int ret = 0;

    if (new_period_ns < current_period_ns)
    {
        // Tono più alto: prima duty_cycle poi period
        ret |= sysfs_write(PWM_CHANNEL_PATH "/duty_cycle", buf_duty);
        ret |= sysfs_write(PWM_CHANNEL_PATH "/period",     buf_period);
    }
    else
    {
        // Tono più basso o uguale: prima period poi duty_cycle
        ret |= sysfs_write(PWM_CHANNEL_PATH "/period",     buf_period);
        ret |= sysfs_write(PWM_CHANNEL_PATH "/duty_cycle", buf_duty);
    }

    if (ret == 0)
    {
        current_period_ns = new_period_ns;
    }

    return ret;
}

/*
=====================================
    FUNZIONI PUBBLICHE
=====================================
*/

int buzzer_pwm_setup(void)
{
    // Esporta il canale PWM (può già essere esportato: in quel caso il kernel
    // restituisce EBUSY, che ignoriamo volutamente)
    int fd = open(PWM_CHIP_PATH "/export", O_WRONLY);
    if (fd < 0)
    {
        perror(PWM_CHIP_PATH "/export");
        return -1;
    }
    write(fd, PWM_CHANNEL, strlen(PWM_CHANNEL));    // ignoriamo EBUSY deliberatamente
    close(fd);

    // Imposta period e duty_cycle di default
    // Il canale è appena esportato quindi current_period_ns != new → usiamo il ramo
    // "tono più basso" (period cresce da 0): prima period poi duty
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", PWM_DEFAULT_PERIOD_NS);
    if (sysfs_write(PWM_CHANNEL_PATH "/period", buf) != 0)
        return -1;

    snprintf(buf, sizeof(buf), "%u", PWM_DEFAULT_PERIOD_NS / 2);
    if (sysfs_write(PWM_CHANNEL_PATH "/duty_cycle", buf) != 0)
        return -1;

    current_period_ns = PWM_DEFAULT_PERIOD_NS;

    return 0;
}

int buzzer_pwm_enable(void)
{
    return sysfs_write(PWM_CHANNEL_PATH "/enable", "1");
}

int buzzer_pwm_disable(void)
{
    return sysfs_write(PWM_CHANNEL_PATH "/enable", "0");
}

int buzzer_pwm_tone_up(void)
{
    if (current_period_ns <= PWM_MIN_PERIOD_NS)
        return 1;   // già al tono massimo

    uint32_t new_period = current_period_ns - PWM_TONE_STEP_NS;

    // Clamp al limite minimo
    if (new_period < PWM_MIN_PERIOD_NS)
        new_period = PWM_MIN_PERIOD_NS;

    return pwm_set_period(new_period);
}

int buzzer_pwm_tone_down(void)
{
    if (current_period_ns >= PWM_MAX_PERIOD_NS)
        return 1;   // già al tono minimo

    uint32_t new_period = current_period_ns + PWM_TONE_STEP_NS;

    // Clamp al limite massimo
    if (new_period > PWM_MAX_PERIOD_NS)
        new_period = PWM_MAX_PERIOD_NS;

    return pwm_set_period(new_period);
}

uint32_t buzzer_pwm_get_period(void)
{
    return current_period_ns;
}