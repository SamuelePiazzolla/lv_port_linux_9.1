#define _POSIX_C_SOURCE 199309L

#include "cameraLogic.h"
#include "../../logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "libyuv.h"     //Libreria per velocizzare conversione YUYV --> XRGB

#ifdef __cplusplus
}

#endif
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <pthread.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/
//DISPLAY & CAMERA DIMENSION
#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480
#define DISPLAY_BASE_COLOR 0x00000000  // nero puro in XRGB8888
#define BPP 4

//VIDEO
#define VIDEO_FOLDER_PATH "./mediaTest"             //Modifica questa define inserendo la cartella da cui caricare i video/salvare i video

//CAMERA & MMAP
#define CAM_BUFFERS 4

//REC CAMERA MAX FRAMES PER BUFFERS
#define MAX_FRAMES_PER_BUFFER 10

/* 
=======================================
    VARIABILI GLOBALI
=======================================
*/
// Variabili per la gestione del displayer video
extern lv_obj_t * ui_videoDisplayer;                                // Mi prendo da fuori il widget image su cui poi andrò a scrivere
static uint32_t cameraFrameBuffer[CAMERA_WIDTH * CAMERA_HEIGHT];    // Buffer per i frame della camera, a 32BPP
static lv_img_dsc_t cameraImgDsc;                                   // Descrittore immagine per LVGL
static bool useCamera = false;                                      // Variabile per sapere se sto prendendo da un file o dalla telecamera

//Variabile timer per simulare il flusso video
static lv_timer_t * g_img_timer = NULL;                             // Puntatore al timer per il video stream

//LETTURA FA FILE
static uint64_t last_ts_ms = 0;                                     // mantiene il timestamp dell'ultimo frame visualizzato
static lv_obj_t * filePicker = NULL;                                // Variabili per la gestione del file picker pop-up
static char selectedVideoPath[512] = {0};                           // Variabile in cui salvare il pathname del video da visualizzare

//TELECAMERA
static int cameraFd = -1;                                           // File descriptor della camera
//mmap
struct
{
    void *start;
    size_t length;
}cam_buffers[CAM_BUFFERS];

//VIDEO RECORDING
typedef struct __attribute__((packed)){                             // L'attribute serve per non far aggiungere padding e avere la dimensione sempre stabile
    uint64_t ts_ms;                 // Timestamp in ms
    uint32_t frame[CAMERA_WIDTH * CAMERA_HEIGHT];  // Frame XRGB8888
} frame_packet_t;                                                   // Struct che rappresenta il pacchetto che scriveremo sul file
static bool recordActive = false;                                   
static pthread_t recordThread;                                      // Thread che si occupa della scrittura su file
static pthread_mutex_t recordMutex = PTHREAD_MUTEX_INITIALIZER;     // Mutex per protezione tra main thread e thread record
static FILE * recordFile = NULL;                                    // File RAW di output
static frame_packet_t recordBuffer[2][MAX_FRAMES_PER_BUFFER];       // Double buffer per frame da scrivere
static bool bufferReady[2] = { false, false };                      // Stato dei buffer (true se pronto da scrivere)
static int activeBufferIndex = 0;                                   // Indice buffer corrente
static size_t framesInBuffer[2] = { 0, 0 };                         // Numero di frame presenti in ciascun buffer

/* 
=======================================
    PROTOTIPI FUNZIONI
=======================================
*/
//generali
static void resetDisplayer(void);                                                           // Funzione per resettare il displayer video ad uno sfondo nero

//Gestione video da file
static bool readFramePacket(FILE* file, frame_packet_t* packet);                            // Funzione per leggere un singolo frame_packet_t dal file RAW
static void readVideoFile(const char * videoPath, int delayMs);                             // Funzione per leggere un file video RAW e visualizzarlo
static void timerCallbackVideo(lv_timer_t * timer);                                         // Funzione spacchetta il contenuto letto dal file, lo mette sul display e modifica il timer con cui viene richiamata

//Pop-up filepicker
static void createFilePicker(void);                                                         // Funzione per creare il pop-up file picker
static void closeFilePicker(lv_event_t * e);                                                // Funzione per chiudere il pop-up file picker da pulsanti
static void destroy_filepicker(void);                                                       // Funzione per eliminare il filepicker correttamente
static void fileSelected(lv_event_t * e);                                                   // Funzione che ti imposta la variabie globale con il path del file selezionato
static void free_filepicker_userdata(lv_obj_t * parent);                                    // Funzione per liberare la memoria occupata per scrivere gli userdata
static void overlay_clicked(lv_event_t * e);                                                // Funzione per chiudere il popup se si preme al di fuori di esso
static void block_event_bubble(lv_event_t * e);                                             // Funzione per evitare la propagazione del click se si preme sul popup

//Gestione video da camera
static void cameraHardwareInit(void);                                                       // Funzione per alimentare la camera e impostare correttamente le pipe
static void cameraClose(void);                                                              // Funzione per chiudere correttamente la telecamera
static void cameraSetup(void);                                                              // Funzione per setup iniziale telecamera
static int cameraOpen(const char *dev);                                                     // Funzione per aprire il dispositivo selezionato
static void cameraCaptureFrame(void);                                                       // Funzione che legge un frame dalla camera e lo copia in cameraBuffer
static void timerCallbackCamera(lv_timer_t * timer);                                        // Funzione per aggiornare LVGL dai frame della camera

/* 
=======================================
    IMPLEMENTAZIONI
=======================================
*/
//GESTIONE DISPLAYER
void resetDisplayer()
{
    for(int i = 0; i < CAMERA_WIDTH * CAMERA_HEIGHT; i++) {
        cameraFrameBuffer[i] = DISPLAY_BASE_COLOR;
    }

    if(ui_videoDisplayer) {
        lv_image_set_src(ui_videoDisplayer, &cameraImgDsc);
    }
}

void createImgDisplayer()
{
    // Crea un oggetto img per visualizzare il video della camera
    if(ui_videoDisplayer == NULL) {
        ERROR_PRINT("Error: Non è presente il video displayer\n");
        return;
    }
    
    //CONFIGURO L'IMAGE DESCRIPTOR
    // Pulisco il framebuffer
    memset(cameraFrameBuffer, 0, sizeof(cameraFrameBuffer));
    
    // Inizializzo il descrittore
    memset(&cameraImgDsc, 0, sizeof(cameraImgDsc));
    
    // Imposto i campi del descrittore immagine
    cameraImgDsc.header.magic = LV_IMAGE_HEADER_MAGIC; // necessario per indicare un lv_image_dsc_t valido
    cameraImgDsc.header.cf = LV_COLOR_FORMAT_XRGB8888; // Formato colore: XRGB8888  32 bpp, lo stesso del frame buffer
    cameraImgDsc.header.flags = 0;
    cameraImgDsc.header.w = CAMERA_WIDTH;
    cameraImgDsc.header.h = CAMERA_HEIGHT;
    cameraImgDsc.header.stride = CAMERA_WIDTH * BPP; //STRIDE A 4 BPP
    cameraImgDsc.reserved = NULL;

    cameraImgDsc.data_size = CAMERA_WIDTH * CAMERA_HEIGHT * BPP; // Dimensione totale dei dati immagine, 32BPP = 4 Byte
    cameraImgDsc.data = (const uint8_t *)cameraFrameBuffer; // Puntatore ai dati dell'immagine

    // Collego il buffer all'immagine già esistente 
    resetDisplayer();

    // Forzo ridisegno
    lv_image_set_src(ui_videoDisplayer, &cameraImgDsc);
    lv_obj_invalidate(ui_videoDisplayer);

    INFO_PRINT("--- VIDEO SCREEN INIZIALIZZATO ---\n");
}

//GESTIONE LETTURA DA FILE
bool readFramePacket(FILE* file, frame_packet_t* packet)
{
    size_t bytesRead = fread(packet, 1, sizeof(frame_packet_t), file);

    if(bytesRead == 0 && feof(file)) {
        return false; // fine file
    }

    if(bytesRead != sizeof(frame_packet_t)) {
        ERROR_PRINT("Warning: incomplete frame read. Expected %zu, got %zu\n", sizeof(frame_packet_t), bytesRead);
        return false;
    }

    return true;
}

void timerCallbackVideo(lv_timer_t * timer)
{
    FILE * file = (FILE *)timer->user_data;
    if(!file) return;

    frame_packet_t packet;

    // Legge il prossimo frame
    if(!readFramePacket(file, &packet))
    {
        DEBUG_PRINT("End of video file or read error.\n");
        fclose(file);
        lv_timer_set_user_data(timer, NULL);
        lv_timer_delete(timer);
        g_img_timer = NULL;

        resetDisplayer();
        lv_label_set_text(ui_playCameraLabel, "PLAY");
        lv_obj_remove_state(ui_playCameraBtn, LV_STATE_CHECKED);

        last_ts_ms = 0;
        return;
    }

    // Copia il frame nel framebuffer per LVGL
    memcpy(cameraFrameBuffer, packet.frame, sizeof(cameraFrameBuffer));

    // Aggiorna l'immagine LVGL
    lv_image_set_src(ui_videoDisplayer, &cameraImgDsc);
    lv_obj_invalidate(ui_videoDisplayer);

    // Calcolo delay per il prossimo timer
    uint32_t delay_ms = 33; // default ~30fps se è il primo frame
    if(last_ts_ms != 0 && packet.ts_ms > last_ts_ms)
    {
        delay_ms = packet.ts_ms - last_ts_ms;
        if(delay_ms == 0) delay_ms = 1; // min 1ms per sicurezza
    }

    last_ts_ms = packet.ts_ms;

    // Aggiorna il timer LVGL con il nuovo intervallo
    lv_timer_set_period(timer, delay_ms);
}

void readVideoFile(const char * videoPath, int delayMs)
{
    if(ui_videoDisplayer == NULL) {
        ERROR_PRINT("Error: Video displayer not created.\n");
        return;
    }

    //Cancella il timer precedente se esiste
    if(g_img_timer != NULL) {
        lv_timer_set_user_data(g_img_timer, NULL);
        lv_timer_delete(g_img_timer);
        g_img_timer = NULL;
    }

    //Apro il file in lettura
    FILE * file = fopen(videoPath, "rb");
    if(file == NULL) {
        ERROR_PRINT("Error: Unable to open video file: %s\n", videoPath);
        return;
    }

    // Crea un timer che legge i frame dal file video
    // Impostiamo inizialmente 1ms, sarà aggiornato dinamicamente nel callback
    g_img_timer = lv_timer_create(timerCallbackVideo, 1, file);
}

//GESTIONE FILE PICKER
void createFilePicker(void)
{
    //Overlay scuro fullscreen
    filePicker = lv_obj_create(lv_layer_top());             //Creo il layer scuro in cui andrò ad inserire il popup
    lv_obj_set_size(filePicker, lv_pct(100), lv_pct(100));  
    lv_obj_set_style_bg_color(filePicker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(filePicker, LV_OPA_70, 0);      //Semi trasparente
    lv_obj_remove_flag(filePicker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(filePicker, overlay_clicked, LV_EVENT_CLICKED, NULL);   //Se premo sul layout sotto al pop chiudo il popup

    //Container centrale
    lv_obj_t * cont = lv_obj_create(filePicker);        //Creo il contenitore che rappresenterà il mio file picker all'interno del layer scurp
    lv_obj_set_size(cont, 640, 480);
    lv_obj_center(cont);
    lv_obj_set_style_radius(cont, 10, 0);
    lv_obj_set_style_bg_color(cont, lv_color_white(), 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_add_event_cb(cont, block_event_bubble, LV_EVENT_ALL, NULL);  //Evito che i clicchi effettuati sul pop-up si propaghino ai genitori

    lv_obj_t * header = lv_obj_create(cont);
    lv_obj_set_size(header, lv_pct(100), 50);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x14191E), 0);
    lv_obj_set_style_pad_all(header, 10, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(header);
    lv_label_set_text(title, "LOAD VIDEO");
    lv_obj_set_style_text_color(title, lv_color_hex3(0xEDEDED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0);
    lv_obj_center(title);

    // Lista scrollabile dei file
    lv_obj_t * list = lv_list_create(cont);
    lv_obj_set_size(list, 620, 380);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 10);
    lv_obj_set_style_pad_all(list, 5, 0);

    // Leggi i file dalla cartella
    DIR *dir = opendir(VIDEO_FOLDER_PATH);
    if(dir) 
    {
        struct dirent *entry;
        while((entry = readdir(dir)) != NULL) 
        {
            //Escludo . e .. 
            if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            // Solo file RAW
            const char * ext = strrchr(entry->d_name, '.');
            if(ext && strcmp(ext, ".raw") == 0)
            {
                lv_obj_t * btn = lv_list_add_button(list, NULL, entry->d_name);
                lv_obj_set_user_data(btn, strdup(entry->d_name));
                lv_obj_add_event_cb(btn, fileSelected, LV_EVENT_CLICKED, NULL);
            }
        }
        closedir(dir);
    } else 
    {
        ERROR_PRINT("Error: unable to open directory %s\n", VIDEO_FOLDER_PATH);
    }

    // Bottone Close (temporaneo)
    lv_obj_t * btn_close = lv_btn_create(cont);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_event_cb(btn_close, closeFilePicker, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_center(lbl_close);

    //Se è andato tutto bene attivo i bottoni altrimenti il tengo disattivati
    if(filePicker != NULL && cont != NULL && title != NULL && btn_close != NULL && lbl_close != NULL)
    {
        DEBUG_PRINT("Creazione pop-up avvenuta con successo\n");
        lv_obj_remove_state(ui_playCameraBtn, LV_STATE_DISABLED);
        lv_obj_remove_state(ui_resetCameraBtn, LV_STATE_DISABLED);
    }
    else
    {
        
        //Disattiva i pulsanti per gestione video
        lv_obj_add_state(ui_playCameraBtn, LV_STATE_DISABLED);
        lv_obj_add_state(ui_resetCameraBtn, LV_STATE_DISABLED);
        ERROR_PRINT("Error: errore nella creazione del pop-up\n");
    }

    
}

void closeFilePicker(lv_event_t * e)
{
    destroy_filepicker();
}

void fileSelected(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    const char * filename = lv_obj_get_user_data(btn);

    if(filename == NULL)
    {
        ERROR_PRINT("Error: filename missing in user_data\n");
        return;
    }

    snprintf(selectedVideoPath, sizeof(selectedVideoPath), "%s/%s", VIDEO_FOLDER_PATH, filename);

    DEBUG_PRINT("File selected: %s\n", selectedVideoPath);

    destroy_filepicker();
}

void overlay_clicked(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        destroy_filepicker();
    }
}

void block_event_bubble(lv_event_t * e)
{
    lv_event_stop_bubbling(e);
}

//Pulisce gli userdata del pop-up in maniera ricorsiva per evitare memory leak  
void free_filepicker_userdata(lv_obj_t * obj)
{
    if(!obj) return;

    void * ud = lv_obj_get_user_data(obj);
    if(ud) {
        free(ud);
        lv_obj_set_user_data(obj, NULL);
    }

    uint32_t cnt = lv_obj_get_child_cnt(obj);
    for(uint32_t i = 0; i < cnt; i++) {
        free_filepicker_userdata(lv_obj_get_child(obj, i));
    }
}

void destroy_filepicker(void)
{
    if(filePicker)
    {
        free_filepicker_userdata(filePicker);
        lv_obj_del(filePicker);
        filePicker = NULL;
        DEBUG_PRINT("File picker closed and destroyed\n");
    }
}

//GESTIONE LETTURA DA VIDEOCAMERA
void cameraHardwareInit(void)
{
    int ret;

    // 1. Abilitazione PWM per la telecamera / backlight
    ret = system("echo 2 > /sys/class/pwm/pwmchip0/export");
    ret |= system("echo 100000 > /sys/class/pwm/pwmchip0/pwm2/period");
    ret |= system("echo 90000 > /sys/class/pwm/pwmchip0/pwm2/duty_cycle");
    ret |= system("echo 1 > /sys/class/pwm/pwmchip0/pwm2/enable");

    // 2. Accensione backlight display
    ret |= system("echo 1 > /sys/class/leds/backlight_en/brightness");

    // 3. Reset media controller
    ret |= system("media-ctl -d /dev/media0 -r");

    // 4. Collegamento pipeline CSI2 -> CRU output
    ret |= system("media-ctl -d /dev/media0 -l \"'rzg2l_csi2 10830400.csi2':1 -> 'CRU output':0 [1]\"");

    // 5. Formato ingresso CSI2
    ret |= system("media-ctl -d /dev/media0 -V \"'rzg2l_csi2 10830400.csi2':1 [fmt:SRGGB8_1X8/640x480 field:none]\"");

    // 6. Formato uscita telecamera
    ret |= system("media-ctl -d /dev/media0 -V \"'tevs 1-0048':0 [fmt:UYVY8_2X8/640x480 field:none]\"");

    if(ret != 0) {
        ERROR_PRINT("Warning: Some hardware initialization commands failed.\n");
    } else {
        INFO_PRINT("--- CAMERA INIZIALIZZATA ---\n");
    }
}

int cameraOpen(const char *dev)
{
    return open(dev, O_RDWR);
}

void cameraSetup()
{
    if(cameraFd >= 0)
    {
        DEBUG_PRINT("Camera already initialized\n");
        return;
    }

    struct v4l2_format fmt;             
    struct v4l2_requestbuffers req;     
    struct v4l2_buffer buf;         
    //Inizializzo i valori del buffer per evitare problemi
    for (size_t i = 0; i < CAM_BUFFERS; i++)
    {
        cam_buffers[i].start = NULL; 
        cam_buffers[i].length = 0;
    }    

    // 1. Apertura device video
    cameraFd = cameraOpen("/dev/video0");
    if(cameraFd < 0) {
        ERROR_PRINT("Error opening camera: %s\n", strerror(errno));
        cameraClose();  // Cleanup in caso di errore
        return;
    }

    // 2. Imposto il formato della telecamera
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAMERA_WIDTH;
    fmt.fmt.pix.height = CAMERA_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if(ioctl(cameraFd, VIDIOC_S_FMT, &fmt) < 0) {
        ERROR_PRINT("Error setting camera format (YUYV): %s\n", strerror(errno));
        cameraClose();
        return;
    }

    // 3. Richiesta buffer mmap
    memset(&req, 0, sizeof(req));
    req.count = CAM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(ioctl(cameraFd, VIDIOC_REQBUFS, &req) < 0) {
        ERROR_PRINT("Error requesting buffer: %s\n", strerror(errno));
        cameraClose();
        return;
    }

    //MAPPING AND QUEUYIRNG BUFFERS
    for(int i = 0; i < CAM_BUFFERS; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        // Querybuf
        if(ioctl(cameraFd, VIDIOC_QUERYBUF, &buf) < 0) {
            ERROR_PRINT("Error querying buffer %d: %s\n", i, strerror(errno));
            cameraClose();
            return;
        }

        // 4. Mappattura di <CAM_BUFFERS>(4) buffer
        cam_buffers[i].length = buf.length;
        cam_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cameraFd, buf.m.offset);

        if(cam_buffers[i].start == MAP_FAILED)
        {
            ERROR_PRINT("Error mapping the buffer %i", i);
            cameraClose();
            return;
        }

        // 5. Queuying buffer 
        if(ioctl(cameraFd, VIDIOC_QBUF, &buf) < 0) 
        {
            ERROR_PRINT("Error queue buffer %d: %s\n", i, strerror(errno));
            cameraClose();
            return;
        }

    } 

    // 6. Starting the stream
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(ioctl(cameraFd, VIDIOC_STREAMON, &type) < 0) 
    {
        ERROR_PRINT("STREAMON FAILED\n");
        cameraClose();
        return;
    }

    // 7. Timer LVGL
    if(g_img_timer != NULL) {
        lv_timer_set_user_data(g_img_timer, NULL);
        lv_timer_delete(g_img_timer);
    }
    g_img_timer = lv_timer_create(timerCallbackCamera, 1000/30, NULL); // 1000/30 = 30fps

    INFO_PRINT("Camera setup completed successfully.\n");
}

void timerCallbackCamera(lv_timer_t * timer)
{
    cameraCaptureFrame();
    if(ui_videoDisplayer) 
    {
        //lv_image_set_src(ui_videoDisplayer, &cameraImgDsc);
        lv_obj_invalidate(ui_videoDisplayer);
    }
}

void cameraCaptureFrame()
{
    if(cameraFd < 0) return;

    // --- LOGICA FPS ---
    static uint32_t frame_count = 0;
    static uint32_t last_ms = 0;
    
    // Otteniamo il tempo attuale in millisecondi
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now_ms = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

    frame_count++;

    // Ogni 1000ms (1 secondo) stampiamo e resettiamo
    if(now_ms - last_ms >= 1000) 
    {
        float fps = (frame_count * 1000.0f) / (now_ms - last_ms);
        DEBUG_PRINT("Actual FPS: %.2f\n", fps);
        
        frame_count = 0;
        last_ms = now_ms;
    }
    // ------------------

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if(ioctl(cameraFd, VIDIOC_DQBUF, &buf) < 0) return;

    if(cam_buffers[buf.index].start == NULL) return; // evita crash su LibYUV

    // --- USA LIBYUV QUI ---
    // Converte YUYV (2 byte per pixel) in ARGB (4 byte per pixel)
    YUY2ToARGB(
        (const uint8_t *)cam_buffers[buf.index].start, // Source YUYV
        CAMERA_WIDTH * 2,                              // Source stride (2 bytes per pixel)
        (uint8_t *)cameraFrameBuffer,                  // Destination ARGB
        CAMERA_WIDTH * 4,                              // Destination stride (4 bytes per pixel)
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    );

    if(ioctl(cameraFd, VIDIOC_QBUF, &buf) < 0) return;

    // RECORDING
    if(recordActive) {
        pthread_mutex_lock(&recordMutex);

        size_t idx = framesInBuffer[activeBufferIndex];
        if(idx < MAX_FRAMES_PER_BUFFER) {
            recordBuffer[activeBufferIndex][idx].ts_ms = now_ms;
            memcpy(recordBuffer[activeBufferIndex][idx].frame, cameraFrameBuffer, sizeof(cameraFrameBuffer));
            framesInBuffer[activeBufferIndex]++;
        }

        // Se il buffer è pieno, lo segnalo come pronto e passo all'altro buffer
        if(framesInBuffer[activeBufferIndex] >= MAX_FRAMES_PER_BUFFER) {
            bufferReady[activeBufferIndex] = true;
            activeBufferIndex = 1 - activeBufferIndex; // Switch buffer
            framesInBuffer[activeBufferIndex] = 0;
        }

        pthread_mutex_unlock(&recordMutex);
    }
}

void cameraClose()
{
    if(cameraFd >= 0) 
    {
        //STOP STREAM
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(ioctl(cameraFd, VIDIOC_STREAMOFF, &type) < 0) 
        {
            ERROR_PRINT("Warning: Error stopping camera stream: %s\n", strerror(errno));
        }

        // Unmap buffers mmap
        for(int i = 0; i < CAM_BUFFERS; i++) 
        {
            if(cam_buffers[i].start && cam_buffers[i].length > 0) 
            {
                munmap(cam_buffers[i].start, cam_buffers[i].length);
                cam_buffers[i].start  = NULL;
                cam_buffers[i].length = 0;
            }
        }
        

        // Close device
        close(cameraFd);
        cameraFd = -1;
        DEBUG_PRINT("Camera closed successfully.\n");
    }

    // Cancella timer se esiste
    if(g_img_timer) 
    {
        lv_timer_set_user_data(g_img_timer, NULL);
        lv_timer_delete(g_img_timer);
        g_img_timer = NULL;
    }

    // Reset display
    resetDisplayer();
}

//GESTIONE SALVATAGGIO DATI SU FILE
void* recordThreadFunc(void *arg)
{
    while(recordActive)
    {
        // Ciclo sui due buffer
        for(int i = 0; i < 2; i++)
        {
            pthread_mutex_lock(&recordMutex);
            bool ready = bufferReady[i];
            size_t nFrames = framesInBuffer[i];
            pthread_mutex_unlock(&recordMutex);

            // Scrivo solo se il buffer è pieno o contiene almeno un frame
            if(ready && nFrames > 0)
            {
                // Aggiorno stato buffer sotto mutex
                pthread_mutex_lock(&recordMutex);
                // Scrittura su file senza flush immediato
                fwrite(recordBuffer[i], sizeof(frame_packet_t), nFrames, recordFile);
                bufferReady[i] = false;
                framesInBuffer[i] = 0;
                pthread_mutex_unlock(&recordMutex);
            }
        }

        // Piccola sleep per ridurre busy wait (1 ms)
        usleep(1000);
    }

    // Alla fine della registrazione faccio flush finale
    if(recordFile) {
        fflush(recordFile);
        fclose(recordFile);
        recordFile = NULL;
    }

    return NULL;
}

//LOGICA PULSANTI
void logic_load_video(void)
{
    //Resetto il sistema
    logic_reset_video();

    //Controllo lo stato del sistema
    if(useCamera == true)
    {
        //Imposto che da adesso ragiono con logica video da file
        useCamera = false;
        
    }

    DEBUG_PRINT("Disattivo pulsante rec\n");
    lv_obj_add_state(ui_recCameraBtn, LV_STATE_DISABLED);   //Disattivo il pulsante rec

    if(filePicker != NULL)
    {
        DEBUG_PRINT("File picker already open\n");
        return;
    }

    DEBUG_PRINT("Opening file picker...\n");
    createFilePicker();
}

void logic_load_camera(void)
{
    //Controllo lo stato del sistema
    if(useCamera == false)
    {
        //Imposto che da adesso ragiono con logica video da telecamera
        useCamera = true;

        //Resetto il sistema
        logic_reset_video();

        //Pulisco, se è stata scritta, la variabile globale che punta al video da mostrare
        selectedVideoPath[0] = '\0';
    }

    if(cameraFd >= 0)
    {
        DEBUG_PRINT("Restarting the camera...\n");
        cameraClose();
    }

    DEBUG_PRINT("Opening the camera...\n");
    cameraHardwareInit();
    usleep(200000); // 200 ms per fare in modo che venga settato tutto 
    cameraSetup();
    lv_obj_remove_state(ui_playCameraBtn, LV_STATE_DISABLED);
    lv_obj_remove_state(ui_resetCameraBtn, LV_STATE_DISABLED);
    lv_obj_remove_state(ui_recCameraBtn, LV_STATE_DISABLED);
    lv_label_set_text(ui_playCameraLabel, "PAUSE");
    lv_obj_add_state(ui_playCameraBtn, LV_STATE_CHECKED);
}

void logic_rec_video(void)
{
    if(useCamera == true && cameraFd >= 0)
    {
        INFO_PRINT("Start recording the camera video...\n");
        lv_label_set_text(ui_recCameraLabel, "STOP REC");
        
        //Controllo inutile, non dovrebbe mai succedere
        if(recordActive) 
        {
            DEBUG_PRINT("Recording is already active!\n");
            return;
        }

        //Controllo eventuali spam su rec
        if (recordFile != NULL) 
        {
            ERROR_PRINT("Error: previous record file still open!\n");
            return;
        }

        // 1. Apri file di output
        char filename[512];
        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        strftime(filename, sizeof(filename), VIDEO_FOLDER_PATH "/video_%Y%m%d_%H%M%S.raw", tm_info);

        recordFile = fopen(filename, "wb");
        if(!recordFile) 
        {
            ERROR_PRINT("Error opening record file: %s\n", strerror(errno));
            return;
        }

        // 2. Imposta flag attivo
        recordActive = true;

        // 3. Inizializza buffer
        pthread_mutex_lock(&recordMutex);
        memset(bufferReady, 0, sizeof(bufferReady));
        memset(framesInBuffer, 0, sizeof(framesInBuffer));
        activeBufferIndex = 0;
        pthread_mutex_unlock(&recordMutex);

        // 4. Lancia il thread di scrittura
        if(pthread_create(&recordThread, NULL, recordThreadFunc, NULL) != 0) 
        {
            ERROR_PRINT("Error: could not create record thread\n");
            recordActive = false;
            fclose(recordFile);
            recordFile = NULL;
            return;
        }

        INFO_PRINT("Recording started: %s\n", filename);
    }
}

void logic_stop_rec_video(void)
{
    if(recordActive == true)
    {
        DEBUG_PRINT("Stopping recording\n");
        lv_label_set_text(ui_recCameraLabel, "REC");
        recordActive = false;
        pthread_join(recordThread, NULL); // aspetta che il thread finisca

        //Pulisco variabili globali
        framesInBuffer[0] = framesInBuffer[1] = 0;
        bufferReady[0] = bufferReady[1] = false;
        activeBufferIndex = 0;
        
        //In teoria viene già fatto nel thread ma non si sa mai
        if (recordFile) 
        {
            fclose(recordFile);
            recordFile = NULL;
        }

    }
    
}

void logic_start_video(void)
{
    //SEZIONE VIDEO DA FILE SYSTEM
    if(useCamera == false)
    {
        //controllo di aver selezionato un video
        if(strlen(selectedVideoPath) == 0) 
        {
            ERROR_PRINT("Error: No video selected yet.\n");
            lv_label_set_text(ui_playCameraLabel, "PLAY");
            return;
        }
        //Se esiste un timer lo riattivo
        if(g_img_timer != NULL) 
        {
            lv_timer_resume(g_img_timer);
            DEBUG_PRINT("Resuming existing video..\n");
            lv_label_set_text(ui_playCameraLabel, "PAUSE");
            return;
        }

        //Carico un file video di test RAW
        readVideoFile(selectedVideoPath, 1000/30); // immagini a 30 fps 1000/30   
        INFO_PRINT("Video from file started (video-timer-based).\n");
    }

    //SEZIONE VIDEO DA TELECAMERA
    if (useCamera == true)
    {
        DEBUG_PRINT("Ricomincio l'acquisizione dalla telecamera...\n");
        //Se esiste un timer lo riattivo
        if(g_img_timer != NULL) 
        {
            lv_timer_resume(g_img_timer);
            DEBUG_PRINT("Resuming existing video..\n");
            lv_label_set_text(ui_playCameraLabel, "PAUSE");
            return;
        }
    }
    lv_label_set_text(ui_playCameraLabel, "PAUSE");
}    

void logic_stop_video(void)
{
    // Implement the logic to stop the video test
    DEBUG_PRINT("Stopping button test...\n");

    //Metto in pausa il video se attivo, controllando che il timer esista
    if(g_img_timer != NULL) 
    {
        lv_timer_pause(g_img_timer);
        DEBUG_PRINT("Video paused.\n");
    }
    else 
    {
        DEBUG_PRINT("No active video to pause.\n");
    }

    lv_label_set_text(ui_playCameraLabel, "RESUME");
}

void logic_reset_video(void)
{
    //Cancella il timer se è attivo
    if(useCamera == true)
    {
        DEBUG_PRINT("Closing camera and resetting displayer\n");

        // ferma thread prima di cameraClose()
        logic_stop_rec_video(); 
        cameraClose();
        
        //Resetto i pulsanti 
        lv_obj_add_state(ui_playCameraBtn, LV_STATE_DISABLED);
        lv_obj_add_state(ui_resetCameraBtn, LV_STATE_DISABLED);
        lv_obj_add_state(ui_recCameraBtn, LV_STATE_DISABLED);
    }
    else
    {
        DEBUG_PRINT("Closing camera and resetting video\n");
        cameraClose();
        if(g_img_timer != NULL) 
        {
            lv_timer_set_user_data(g_img_timer, NULL);
            lv_timer_delete(g_img_timer);
            g_img_timer = NULL;
        }
    }

    INFO_PRINT("Reset completed.\n");

    lv_label_set_text(ui_playCameraLabel, "PLAY");
    lv_obj_remove_state(ui_playCameraBtn, LV_STATE_CHECKED);
}

//LOGICA USCITA DAL VIDEO
void logic_deinit_camera_screen(void)
{
    DEBUG_PRINT("Ripulisco tutto all'uscita dalla schermata\n");

    // Ferma video / registrazione / timer / camera
    logic_stop_rec_video();  // ferma registrazione se attiva
    if(cameraFd >= 0 || useCamera) {
        cameraClose();        // ferma camera e timer
    }

    // Reset displayer
    memset(cameraFrameBuffer, 0, sizeof(cameraFrameBuffer)); // nero puro
    resetDisplayer();  // aggiorna LVGL

    // Pulizia variabili globali
    selectedVideoPath[0] = '\0';    // cancella il percorso del video selezionato
    last_ts_ms = 0;                 // reset timestamp
    useCamera = false;              // reset modalità
    g_img_timer = NULL;             // il timer già eliminato in cameraClose
    recordActive = false;           // flag registrazione
    recordFile = NULL;              // file chiuso in logic_stop_rec_video
    activeBufferIndex = 0;
    framesInBuffer[0] = framesInBuffer[1] = 0;
    bufferReady[0] = bufferReady[1] = false;

    // Reset pulsanti (all'apertura dello schermo saranno solo i load abilitati)
    lv_obj_add_state(ui_playCameraBtn, LV_STATE_DISABLED);
    lv_obj_add_state(ui_resetCameraBtn, LV_STATE_DISABLED);
    lv_obj_add_state(ui_recCameraBtn, LV_STATE_DISABLED);

    // Reset label PLAY/REC
    lv_label_set_text(ui_playCameraLabel, "PLAY");
    lv_label_set_text(ui_recCameraLabel, "REC");
    lv_obj_remove_state(ui_playCameraBtn, LV_STATE_CHECKED);
    lv_obj_remove_state(ui_recCameraBtn, LV_STATE_CHECKED);

    // Distruggi eventuale file picker
    destroy_filepicker();

    INFO_PRINT("--- VIDEO E CAMERA SCREEN DEINIZIALIZZATO ---\n");
}
