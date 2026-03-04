#include "../../logic.h"
#include "audioLogic.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/
// CARTELLA FILE AUDIO
#define AUDIO_FOLDER_PATH "mediaDemo/audioDemo/"     // Cartella in cui salvare e trovare le registrazioni

// IMPOSTAZIONI MICROFONO
#define PCM_DEVICE "default"                // Tipo di dispositivo PCM
#define SAMPLE_RATE 48000                   // Rateo di campionamento
#define CHANNELS 2                          // Numero di canali
#define BUFFER_FRAMES 1024

// AUDIO DA FILE
#define PLAYBACK_BUFFER_SIZE (BUFFER_FRAMES * 2 * 2)   // bytes da leggere per volta


/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/

// MICROFONO
typedef struct {
    snd_pcm_t *pcm_handle;
    int16_t *buffer;                        // buffer dinamico
    snd_pcm_uframes_t period_size;          // frames per periodo
    atomic_bool running;                    // stato sistema
    pthread_t thread;
    FILE *file;                             // file in cui scrivere
    uint32_t data_bytes_written;
} AudioRecorder;

static AudioRecorder recorder;              // Variabile che contiene tutti gli elementi necessari per registrare un file audio

// WAV HEADER
typedef struct __attribute__((packed)) {
    char riff_id[4];        // "RIFF"
    uint32_t riff_size;     // 36 + data_size
    char wave_id[4];        // "WAVE"

    char fmt_id[4];         // "fmt "
    uint32_t fmt_size;      // 16
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;

    char data_id[4];        // "data"
    uint32_t data_size;
} WavHeader;                                // Struct per header file formato WAV

// FILE PICKER
static lv_obj_t *audioFilePicker = NULL;    // Finestra per filepicker
static char selectedAudioPath[512] = {0};   // Percorso file selezionato

// AUDIO DA FILE
typedef struct {
    pthread_t thread;

    atomic_bool running;
    atomic_bool paused;
    atomic_bool thread_alive;

    char filepath[512];
} AudioPlayer;

static AudioPlayer player;

// FLAG PER THREAD
static atomic_bool record_thread_alive;     // Variabile per vedere se il thread di record è attivo


/* 
=======================================
    PROTOTIPI FUNZIONI
=======================================
*/

// MICROFONO
static void* record_thread(void *arg);                                  // Funzione per thread di registrazione del microfono
static int  start_recording(const char *filename);                      // Funzione per setup recorder, apertura file e chiamata al thread
static void write_wav_header(FILE *file);                               // Funzione per scrivere l'header WAV all'inizio del file
static void finalize_wav_header(FILE *file, uint32_t data_size);        // Funzione per aggiornamento finale header WAV
static void my_snd_pcm_exit(void);                                      // Funzione per corretta chiusura PCM
static int  my_error_init(void);                                        // Funzione di cleanup in caso di errore in start_recording
static void stop_recording_async(void);                                 // Funzione per terminare correttamente il thread senza bloccare la GUI

// FILE PICKER
static void createAudioFilePicker(void);                                // Funzione per creare il picker
static void destroy_audio_filepicker(void);                             // Funzione per eliminare il picker
static void audioFileSelected(lv_event_t *e);                           // Funzione per ottenere il file selezionato tramite il picker
static void overlay_clicked_audio(lv_event_t *e);                       // Funzione per chiudere il picker se viene cliccato all'esterno di esso
static void block_event_bubble_audio(lv_event_t *e);                    // Funzione per evitare che i click sul picker passino agli elementi sottostanti
static void free_audio_filepicker_userdata(lv_obj_t *obj);              // Funzione per pulire correttamente le variabili utilizzate per il picker

// LETTURA DA FILE
static void* playback_thread(void *arg);                                // Funzione per leggere i dati da far uscire sulle casse in un thread separato
static int  audio_play(const char *path);                               // Funzione per cominciare la riproduzione audio
static void audio_toggle_pause(void);                                   // Funzione per passare da stato paused a resumed e viceversa
static void audio_reset(void);                                          // Funzione per fermare e resettare il player

// GESTIONE VOLUME
static void set_system_volume(int percent);                             // Funzione per gestire il volume delle casse da GUI
static void audio_hw_init(void);                                        // Funzione per inizializzare alcuni valori audio hardware

// UTILITY
static void wait_thread_exit(atomic_bool *alive_flag, pthread_t thread, int timeout_ms);    // Funzione per aspettare la fine dei thread senza bloccare UI, se scade il timeout fallback su join
static void playback_finished_cb(void *arg);                                                // Serve per modificare la GUI quando la riproduzione audio finisce


/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/

/* -------------------------------------
 *  UTILITY
 *------------------------------------- */

static void wait_thread_exit(atomic_bool *alive_flag, pthread_t thread, int timeout_ms)
{
    const int step_us = 500;
    int waited = 0;

    while (atomic_load(alive_flag) && waited < timeout_ms * 1000)
    {
        usleep(step_us);
        waited += step_us;
    }

    if (atomic_load(alive_flag))
    {
        ERROR_PRINT("[AUDIO] Timeout thread, fallback su pthread_join\n");
        pthread_join(thread, NULL);
    }
}

static void playback_finished_cb(void *arg)
{
    (void)arg;

    // Resetto tutti i pulsanti allo stato di default
    lv_label_set_text(ui_playStopAudioBtnLabel, "PLAY");
    lv_obj_remove_state(ui_playStopAudioBtn, LV_STATE_CHECKED);
    lv_obj_remove_state(ui_outAudioLed, LV_STATE_CHECKED);
}


/* -------------------------------------
 *  RIPRODUZIONE DA FILE
 * ------------------------------------- */

static void* playback_thread(void *arg)
{
    const char *filepath = (const char *)arg;

    snd_pcm_t *pcm  = NULL;
    FILE      *file = NULL;
    int16_t    buffer[BUFFER_FRAMES * CHANNELS];
    int        err;

    atomic_store(&player.thread_alive, true);

    /* --- apri file --- */
    file = fopen(filepath, "rb");
    if (!file) goto cleanup;

    /* Salta header WAV */
    fseek(file, sizeof(WavHeader), SEEK_SET);

    /* --- apri ALSA --- */
    err = snd_pcm_open(&pcm, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) goto cleanup;

    /* --- hw params --- */
    snd_pcm_set_params(
        pcm,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        CHANNELS,
        SAMPLE_RATE,
        1,
        500000
    );

    /* --- playback loop --- */
    while (atomic_load(&player.running))
    {
        if (atomic_load(&player.paused))
        {
            usleep(10 * 1000);
            continue;
        }

        size_t r = fread(buffer, sizeof(int16_t), BUFFER_FRAMES * CHANNELS, file);
        if (r == 0) break;  // EOF

        snd_pcm_uframes_t  frames = r / CHANNELS;
        snd_pcm_sframes_t  w      = snd_pcm_writei(pcm, buffer, frames);
        if (w < 0) snd_pcm_recover(pcm, w, 1);
    }

    /* --- svuoto ALSA prima di notificare la GUI --- */
    if (atomic_load(&player.running))
        snd_pcm_drain(pcm);     // drain solo se fine naturale; skip se reset

cleanup:
    if (pcm)  { snd_pcm_drop(pcm); snd_pcm_close(pcm); }
    if (file) { fclose(file); }

    atomic_store(&player.thread_alive,   false);
    atomic_store(&player.running,        false);
    atomic_store(&player.paused,         false);

    /* Notifica la GUI in modo thread-safe tramite LVGL async */
    lv_async_call(playback_finished_cb, NULL);

    return NULL;
}


/* -------------------------------------
 *  API PLAYER
 * ------------------------------------- */

static int audio_play(const char *path)
{
    if (atomic_load(&player.thread_alive))
        return -1;

    snprintf(player.filepath, sizeof(player.filepath), "%s", path);

    atomic_store(&player.running, true);
    atomic_store(&player.paused,  false);

    if (pthread_create(&player.thread, NULL, playback_thread, player.filepath) != 0)
    {
        atomic_store(&player.running, false);
        return -1;
    }

    pthread_detach(player.thread);
    return 0;
}

static void audio_toggle_pause(void)
{
    if (!atomic_load(&player.thread_alive)) 
        return;

    bool p = atomic_load(&player.paused);
    atomic_store(&player.paused, !p);
}

static void audio_reset(void)
{
    if (!atomic_load(&player.thread_alive)) return;

    atomic_store(&player.running, false);
    atomic_store(&player.paused,  false);
}


// -------------------------------------
//  FILE PICKER
// -------------------------------------

static void createAudioFilePicker(void)
{
    audioFilePicker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(audioFilePicker, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(audioFilePicker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(audioFilePicker, LV_OPA_70, 0);
    lv_obj_remove_flag(audioFilePicker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(audioFilePicker, overlay_clicked_audio, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cont = lv_obj_create(audioFilePicker);
    lv_obj_set_size(cont, 640, 480);
    lv_obj_center(cont);
    lv_obj_set_style_radius(cont, 10, 0);
    lv_obj_set_style_bg_color(cont, lv_color_white(), 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_add_event_cb(cont, block_event_bubble_audio, LV_EVENT_ALL, NULL);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "LOAD AUDIO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *list = lv_list_create(cont);
    lv_obj_set_size(list, 620, 380);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);

    DIR *dir = opendir(AUDIO_FOLDER_PATH);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            const char *ext = strrchr(entry->d_name, '.');
            if (!ext) continue;

            if (strcmp(ext, ".wav") == 0)
            {
                char *name_copy = strdup(entry->d_name);
                if (!name_copy) continue;

                lv_obj_t *btn = lv_list_add_button(list, NULL, entry->d_name);
                lv_obj_set_user_data(btn, name_copy);
                lv_obj_add_event_cb(btn, audioFileSelected, LV_EVENT_CLICKED, NULL);
            }
        }
        closedir(dir);
    }
}

static void audioFileSelected(lv_event_t *e)
{
    lv_obj_t *btn      = lv_event_get_target(e);
    char     *filename = (char *)lv_obj_get_user_data(btn);

    if (!filename) return;

    snprintf(selectedAudioPath, sizeof(selectedAudioPath),
             "%s/%s", AUDIO_FOLDER_PATH, filename);

    INFO_PRINT("Audio file selected: %s\n", selectedAudioPath);

    destroy_audio_filepicker();

    // Abilita i bottoni audio
    lv_obj_remove_state(ui_playStopAudioBtn, LV_STATE_DISABLED);
    lv_obj_remove_state(ui_resetAudioBtn,    LV_STATE_DISABLED);

    // Setto il volume di base
    logic_change_volume();
}

static void free_audio_filepicker_userdata(lv_obj_t *obj)
{
    if (!obj) return;

    void *ud = lv_obj_get_user_data(obj);
    if (ud)
    {
        free(ud);
        lv_obj_set_user_data(obj, NULL);
    }

    uint32_t cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < cnt; i++)
        free_audio_filepicker_userdata(lv_obj_get_child(obj, i));
}

static void destroy_audio_filepicker(void)
{
    if (audioFilePicker)
    {
        free_audio_filepicker_userdata(audioFilePicker);
        lv_obj_del(audioFilePicker);
        audioFilePicker = NULL;
        INFO_PRINT("File picker closed and destroyed\n");
    }
}

static void overlay_clicked_audio(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        destroy_audio_filepicker();

        // Rimuovo eventuali selezioni
        selectedAudioPath[0] = '\0';

        // Resetto bottoni
        lv_label_set_text(ui_playStopAudioBtnLabel, "PLAY");

        lv_obj_add_state(ui_playStopAudioBtn, LV_STATE_DISABLED);
        lv_obj_add_state(ui_resetAudioBtn,    LV_STATE_DISABLED);

        lv_obj_remove_state(ui_playStopAudioBtn, LV_STATE_CHECKED);
        lv_obj_remove_state(ui_outAudioLed,      LV_STATE_CHECKED);
    }
}

static void block_event_bubble_audio(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}


/* -------------------------------------
 *  MICROFONO — utility
 * ------------------------------------- */

static void my_snd_pcm_exit(void)
{
    if (recorder.pcm_handle)
    {
        snd_pcm_drop(recorder.pcm_handle);
        snd_pcm_close(recorder.pcm_handle);
        recorder.pcm_handle = NULL;
    }
}

static int my_error_init(void)
{
    my_snd_pcm_exit();

    if (recorder.buffer)
    {
        free(recorder.buffer);
        recorder.buffer = NULL;
    }

    return -1;
}


/* -------------------------------------
 *  GESTIONE WAV
 * ------------------------------------- */

static void write_wav_header(FILE *file)
{
    WavHeader header;

    memcpy(header.riff_id,  "RIFF", 4);
    memcpy(header.wave_id,  "WAVE", 4);
    memcpy(header.fmt_id,   "fmt ", 4);
    memcpy(header.data_id,  "data", 4);

    header.fmt_size        = 16;
    header.audio_format    = 1;     // PCM
    header.num_channels    = CHANNELS;
    header.sample_rate     = SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.byte_rate       = SAMPLE_RATE * CHANNELS * (16 / 8);
    header.block_align     = CHANNELS * (16 / 8);
    header.data_size       = 0;     // placeholder
    header.riff_size       = 36;    // placeholder

    if (fwrite(&header, sizeof(WavHeader), 1, file) != 1)
        ERROR_PRINT("write wav header failed\n");
}

static void finalize_wav_header(FILE *file, uint32_t data_size)
{
    uint32_t riff_size = 36 + data_size;

    fseek(file, 4, SEEK_SET);
    if (fwrite(&riff_size, sizeof(uint32_t), 1, file) != 1)
        ERROR_PRINT("patch riff wav header failed\n");

    fseek(file, 40, SEEK_SET);
    if (fwrite(&data_size, sizeof(uint32_t), 1, file) != 1)
        ERROR_PRINT("patch data size wav header failed\n");
}


/* ------------------------------------
 *  MICROFONO — thread e apertura
 * ------------------------------------- */

static void* record_thread(void *arg)
{
    (void)arg;

    /* 1. Attivo il flag per informare che sto registrando */
    atomic_store(&record_thread_alive, true);

    int num_frame;

    /* 2. Eseguo questo ciclo finchè sto registrando */
    while (atomic_load(&recorder.running))
    {
        num_frame = snd_pcm_readi(recorder.pcm_handle, recorder.buffer, recorder.period_size);

        /* Nessun dato pronto, attendo attendo 1 ms */
        if (num_frame == -EAGAIN)
        {
            usleep(1000);  
            continue;
        }
        else if (num_frame == -EPIPE) /* Overrun, preparo nuovamente il device */
        {
            snd_pcm_prepare(recorder.pcm_handle);   
            continue;
        }
        else if (num_frame < 0) /* Errore, chiudo la registrazione */
        {
            ERROR_PRINT("Error reading PCM: %s\n", snd_strerror(num_frame));
            atomic_store(&recorder.running, false);
            break;
        }
        else if ((snd_pcm_uframes_t)num_frame != recorder.period_size)
        {
            ERROR_PRINT("Short read, got %d frames\n", num_frame);
        }

        if (num_frame > 0)
        {
            size_t bytes   = (size_t)num_frame * CHANNELS * sizeof(int16_t);
            size_t written = fwrite(recorder.buffer, 1, bytes, recorder.file);
            if (written != bytes)
            {
                ERROR_PRINT("fwrite failed\n");
                atomic_store(&recorder.running, false);
                break;
            }
            recorder.data_bytes_written += (uint32_t)written;
        }
    }

    // Cleanup finale
    my_snd_pcm_exit();

    if (recorder.buffer)
    {
        free(recorder.buffer);
        recorder.buffer = NULL;
    }

    if (recorder.file)
    {
        finalize_wav_header(recorder.file, recorder.data_bytes_written);
        fclose(recorder.file);
        recorder.file = NULL;
    }

    atomic_store(&record_thread_alive, false);
    return NULL;
}

static int start_recording(const char *filename)
{
    int err;
    snd_pcm_hw_params_t *params = NULL;

    /* 1. Apro il driver della scheda per iniziare la registrazione */
    err = snd_pcm_open(&recorder.pcm_handle, PCM_DEVICE, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0)
    {
        ERROR_PRINT("Cannot open audio device %s: %s\n", PCM_DEVICE, snd_strerror(err));
        return -1;
    }

    /* 2. Inizializzazione dei parametri del driver */
    if (snd_pcm_hw_params_malloc(&params) < 0)                                              
        goto error;
    if (snd_pcm_hw_params_any(recorder.pcm_handle, params) < 0)                            
        goto error;
    if (snd_pcm_hw_params_set_access(recorder.pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
        goto error;
    if (snd_pcm_hw_params_set_format(recorder.pcm_handle, params, SND_PCM_FORMAT_S16_LE) < 0)
        goto error;
    if (snd_pcm_hw_params_set_channels(recorder.pcm_handle, params, CHANNELS) < 0)        
        goto error;

    err = snd_pcm_hw_params_set_rate(recorder.pcm_handle, params, SAMPLE_RATE, 0);
    if (err < 0)
    {
        ERROR_PRINT("Cannot set sample rate: %s\n", snd_strerror(err));
        goto error;
    }

    /* 3. Imposto il periodo di campionamento più vicino possibile al numero di frame voluti*/
    snd_pcm_uframes_t period_size = BUFFER_FRAMES;
    err = snd_pcm_hw_params_set_period_size_near(recorder.pcm_handle, params, &period_size, 0);
    if (err < 0)
    {
        ERROR_PRINT("Cannot set period size: %s\n", snd_strerror(err));
        goto error;
    }

    INFO_PRINT("Period size effettivo: %lu frames\n", (unsigned long)period_size);
    recorder.period_size = period_size;

    /* 4. Alloco il buffer con il periodo che ci ha fornito il kernel linux */
    recorder.buffer = malloc(period_size * CHANNELS * sizeof(int16_t));
    if (!recorder.buffer)
    {
        ERROR_PRINT("Cannot allocate buffer\n");
        goto error;
    }

    /* 5. Passo i parametri per l'inizializzazione */
    if (snd_pcm_hw_params(recorder.pcm_handle, params) < 0) 
        goto error;

    snd_pcm_hw_params_free(params);
    params = NULL;

    if (snd_pcm_prepare(recorder.pcm_handle) < 0) { goto error; }

    /* 6. Apro il file in cui salvare la registrazione */
    FILE *file = fopen(filename, "wb");
    if (!file)
    {
        ERROR_PRINT("Cannot open file %s for writing\n", filename);
        my_error_init();    // libera buffer e pcm_handle
        return -1;
    }

    /* 7. Inizializzo le variabili per la registrazione */
    recorder.data_bytes_written = 0;
    atomic_store(&recorder.running, true);
    recorder.file = file;

    /* 8. Scrivo l'header WAV all'inizio del file*/
    write_wav_header(recorder.file);

    /* 9. Faccio partire il thread che si occuperà della scrittura del file, passandogli il puntatore al file aperto*/
    if (pthread_create(&recorder.thread, NULL, record_thread, NULL) != 0)
    {
        ERROR_PRINT("pthread_create failed\n");
        fclose(recorder.file);
        recorder.file = NULL;
        my_error_init();
        return -1;
    }

    /* 10. In questo modo non dovrò attendere il thread con il join ma si chiuderà e pulirà correttamente da solo */
    pthread_detach(recorder.thread);
    return 0;

error:
    if (params) snd_pcm_hw_params_free(params);
    my_error_init();    // chiude pcm_handle e libera buffer
    return -1;
}

static void stop_recording_async(void)
{
    atomic_store(&recorder.running, false);
}


/* -------------------------------------
 *  GESTIONE VOLUME
 * ------------------------------------- */

static void set_system_volume(int percent)
{
    snd_mixer_t          *handle = NULL;
    snd_mixer_selem_id_t *sid    = NULL;
    snd_mixer_elem_t     *elem   = NULL;
    long minv, maxv;

    if (snd_mixer_open(&handle, 0) < 0)                           return;
    if (snd_mixer_attach(handle, "default") < 0)                  goto cleanup;
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0)         goto cleanup;
    if (snd_mixer_load(handle) < 0)                               goto cleanup;

    snd_mixer_selem_id_malloc(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Headphone");

    elem = snd_mixer_find_selem(handle, sid);
    if (!elem) goto cleanup;

    if (!snd_mixer_selem_has_playback_volume(elem)) goto cleanup;

    snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);

    long volume = minv + (percent * (maxv - minv) / 100);
    snd_mixer_selem_set_playback_volume_all(elem, volume);

cleanup:
    if (sid)    snd_mixer_selem_id_free(sid);
    if (handle) snd_mixer_close(handle);
}

static void audio_hw_init(void)
{
    // Tabella controlli da inizializzare
    // { nome, valore, is_percent }
    //   is_percent = 1  → il valore è percentuale del range del controllo
    //   is_percent = 0  → il valore è assoluto (es. indice enum o switch)
    static const struct {
        const char *name;
        long        value;
        int         is_percent;
    } controls[] = {
        { "Digital",               90, 1 },   // 90% del range volume
        { "Digital Playback Boost", 1, 0 },   // valore assoluto (+6 dB)
    };
    static const int num_controls = (int)(sizeof(controls) / sizeof(controls[0]));

    snd_mixer_t          *handle = NULL;
    snd_mixer_selem_id_t *sid    = NULL;

    if (snd_mixer_open(&handle, 0) < 0)
    {
        ERROR_PRINT("audio_hw_init: mixer open failed\n");
        return;
    }
    if (snd_mixer_attach(handle, "default") < 0)
    {
        ERROR_PRINT("audio_hw_init: mixer attach failed\n");
        goto cleanup;
    }
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) 
        goto cleanup;
    if (snd_mixer_load(handle) < 0)                       
        goto cleanup;
    if (snd_mixer_selem_id_malloc(&sid) < 0)
    {
        ERROR_PRINT("audio_hw_init: selem_id alloc failed\n");
        goto cleanup;
    }

    for (int i = 0; i < num_controls; i++)
    {
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, controls[i].name);

        snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);
        if (!elem)
        {
            ERROR_PRINT("audio_hw_init: control '%s' not found\n", controls[i].name);
            continue;
        }

        if (snd_mixer_selem_has_playback_volume(elem))
        {
            long vol;
            if (controls[i].is_percent)
            {
                long minv, maxv;
                snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);
                vol = minv + (controls[i].value * (maxv - minv) / 100);
            }
            else
            {
                vol = controls[i].value;
            }
            snd_mixer_selem_set_playback_volume_all(elem, vol);
        }
        else if (snd_mixer_selem_has_playback_switch(elem))
        {
            // Controllo di tipo switch (on/off o enum) → set come switch
            snd_mixer_selem_set_playback_switch_all(elem, (int)controls[i].value);
        }
        else
        {
            ERROR_PRINT("audio_hw_init: control '%s' has no playback volume or switch\n",
                        controls[i].name);
        }
    }

cleanup:
    if (sid)    snd_mixer_selem_id_free(sid);
    if (handle) snd_mixer_close(handle);
}

/* -------------------------------------
 *  LOGICA PULSANTI
 * ------------------------------------- */

void logic_record_audio(void)
{
    INFO_PRINT("Inizio a registrare l'audio...\n");

    /* 1. Cambio lo stato e il testo del pulsante */
    lv_label_set_text(ui_recordmicBtnLabel, "STOP RECORDING");
    lv_obj_add_state(ui_micLed, LV_STATE_CHECKED);


    /* 2. Comincio la registrazione */
    char filename[512];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    strftime(filename, sizeof(filename), AUDIO_FOLDER_PATH "/audio_%Y%m%d_%H%M%S.wav", tm_info);

    if (start_recording(filename) != 0)
    {
        ERROR_PRINT("Errore nella registrazione\n");

        // In caso di errore resetto lo stato dei bottoni
        lv_label_set_text(ui_recordmicBtnLabel, "RECORD MESSAGE");
        lv_obj_remove_state(ui_recordMicBtn, LV_STATE_CHECKED);
        lv_obj_remove_state(ui_micLed,       LV_STATE_CHECKED);

        return;
    }

    INFO_PRINT("Registrazione avviata con successo, file: %s\n", filename);
}

void logic_stop_record_audio(void)
{
    if (atomic_load(&recorder.running))
    {
        INFO_PRINT("Smetto di registrare l'audio...\n");

        lv_label_set_text(ui_recordmicBtnLabel, "RECORD MESSAGE");
        lv_obj_remove_state(ui_micLed, LV_STATE_CHECKED);

        stop_recording_async();
    }
}

void logic_change_volume(void)
{
    int vol = lv_slider_get_value(ui_volumeSlider);
    INFO_PRINT("Cambio di volume, nuovo valore: %d\n", vol);
    set_system_volume(vol);
}

void logic_select_file_audio(void)
{
    DEBUG_PRINT("Opening audio file picker...\n");

    // Resetto sempre il player prima di aprire il picker
    logic_reset_file_audio();

    selectedAudioPath[0] = '\0';

    if (audioFilePicker) return;    // picker già aperto

    createAudioFilePicker();
}

void logic_play_file_audio(void)
{
    if (selectedAudioPath[0] == '\0') return;

    /* Thread non esiste --> avvia da capo */
    if (!atomic_load(&player.thread_alive))
    {
        if (audio_play(selectedAudioPath) == 0)
        {
            lv_label_set_text(ui_playStopAudioBtnLabel, "STOP");
            lv_obj_add_state(ui_outAudioLed, LV_STATE_CHECKED);
        }
        return;
    }

    /* Thread già attivo --> toggle pausa e aggiorna GUI di conseguenza */
    audio_toggle_pause();

    bool is_now_paused = atomic_load(&player.paused);
    if (is_now_paused)
    {
        lv_label_set_text(ui_playStopAudioBtnLabel, "RESUME");
        lv_obj_remove_state(ui_outAudioLed, LV_STATE_CHECKED);
    }
    else
    {
        lv_label_set_text(ui_playStopAudioBtnLabel, "STOP");
        lv_obj_add_state(ui_outAudioLed, LV_STATE_CHECKED);
    }
}

void logic_stop_file_audio(void)
{
    audio_toggle_pause();

    lv_label_set_text(ui_playStopAudioBtnLabel, "RESUME");
    lv_obj_remove_state(ui_outAudioLed, LV_STATE_CHECKED);
}

void logic_reset_file_audio(void)
{
    audio_reset();

    lv_label_set_text(ui_playStopAudioBtnLabel, "PLAY");
    lv_obj_remove_state(ui_playStopAudioBtn, LV_STATE_CHECKED); 
    lv_obj_remove_state(ui_outAudioLed,      LV_STATE_CHECKED);
}


/* -------------------------------------
 * INIT / DEINIT SCHERMO
 * ------------------------------------- */

void logic_init_audio_screen(void)
{
    audio_hw_init();
    INFO_PRINT("----------------------------------\n");
    INFO_PRINT("--- AUDIO SCREEN INIZIALIZZATO ---\n");
    INFO_PRINT("----------------------------------\n");
}

void logic_deinit_audio_screen(void)
{
    /* 1. FERMO REGISTRAZIONE AUDIO */
    if (atomic_load(&recorder.running))
    {
        stop_recording_async();
        wait_thread_exit(&record_thread_alive, recorder.thread, 200);     // max 200 ms
    }

    /* 2. FERMO RIPRODUZIONE AUDIO */
    if (atomic_load(&player.thread_alive))
    {
        audio_reset();
        wait_thread_exit(&player.thread_alive, player.thread, 200);  // max 200 ms (drain ALSA)
    }

    /* 3. CHIUDO FILE PICKER */
    if (audioFilePicker)
        destroy_audio_filepicker();

    /* 4. RESET GUI */
    lv_label_set_text(ui_playStopAudioBtnLabel, "PLAY");
    lv_label_set_text(ui_recordmicBtnLabel,     "RECORD MESSAGE");

    lv_obj_add_state(ui_playStopAudioBtn, LV_STATE_DISABLED);
    lv_obj_add_state(ui_resetAudioBtn,    LV_STATE_DISABLED);

    lv_obj_remove_state(ui_recordMicBtn,     LV_STATE_CHECKED);
    lv_obj_remove_state(ui_playStopAudioBtn, LV_STATE_CHECKED);
    lv_obj_remove_state(ui_micLed,           LV_STATE_CHECKED);
    lv_obj_remove_state(ui_outAudioLed,      LV_STATE_CHECKED);

    /* 5. RESET STATO LOGICO */
    selectedAudioPath[0] = '\0';

    recorder.file       = NULL;
    recorder.pcm_handle = NULL;
    recorder.buffer     = NULL;
    atomic_store(&recorder.running, false);

    lv_slider_set_value(ui_volumeSlider, 50, LV_ANIM_OFF);
    logic_change_volume();

    INFO_PRINT("------------------------------------\n");
    INFO_PRINT("--- AUDIO SCREEN DEINIZIALIZZATO ---\n");
    INFO_PRINT("------------------------------------\n");
}