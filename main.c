#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include "ui/ui.h"
#include "logic/logic.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <glib.h>
#include <pthread.h>

/* 
=======================================
    DEFINIZIONI
=======================================
*/
#define DISP_DEV "/dev/fb0"
#define INPUT_DEV "/dev/input/event1"

// Risoluzione display
#define DISP_HOR_RES 1024
#define DISP_VER_RES 600

// Tick LVGL in ms
#define LV_TICK_PERIOD 5

/* 
=======================================
   PROTOTIPI
=======================================
*/

static uint32_t get_time_ms(void);                                                                    // Restituisce il tempo reale in ms
static int read_touch_range(const char* dev_path, int* min_x, int* max_x, int* min_y, int* max_y);    // Restituisce il min/max RAW del touch
static void* glib_thread_func(void* arg);                                                             // Thread GLib per DBus/BlueZ

/* 
=======================================
   VARIABILI GLOBALI
=======================================
*/

static gboolean keep_glib_loop_running = TRUE;

/* 
=======================================
   FUNZIONI
=======================================
*/

static uint32_t get_time_ms(void) 
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int read_touch_range(const char* dev_path, int* min_x, int* max_x, int* min_y, int* max_y) 
{
    int fd = open(dev_path, O_RDONLY);
    if(fd < 0) 
    { 
      ERROR_PRINT("open input device"); 
      return -1; 
   }

    struct input_absinfo abs;
    if(ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0) 
    { 
      *min_x = abs.minimum; 
      *max_x = abs.maximum; 
   } 
   else 
   { 
      close(fd); 
      return -1; 
   }

   if(ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0) 
   { 
      *min_y = abs.minimum; 
      *max_y = abs.maximum; 
   } 
   else 
   { 
      close(fd); 
      return -1; 
   }

   close(fd);
   return 0;
}

static void* glib_thread_func(void* arg) 
{
   GMainContext* context = g_main_context_default();  // usa il main context di default
   while (keep_glib_loop_running) 
   {
      // Processa eventi GLib senza bloccare
      g_main_context_iteration(context, FALSE);
      usleep(5000); // 5ms pausa per non saturare la CPU
   }
   return NULL;
}

int main(void) 
{
   /* 1. Inizializzo LVGL */
   lv_init();

   /* 2. Calcolo dimensione stride e buffer per framebuffer 32-bit */
   uint32_t stride = DISP_HOR_RES * sizeof(uint32_t); // stride in byte
   uint32_t buf_size_bytes = stride * DISP_VER_RES;

   /* 3. Allocazione buffer statico FULL render mode */
   static uint32_t fb_buf1[DISP_HOR_RES * DISP_VER_RES];
   static uint32_t fb_buf2[DISP_HOR_RES * DISP_VER_RES];
   memset(fb_buf1, 0, sizeof(fb_buf1));
   memset(fb_buf2, 0, sizeof(fb_buf2));

   /* 4. Configuro display framebuffer Linux */
   lv_display_t *dispp = lv_linux_fbdev_create();
   if(!dispp) 
   {
      ERROR_PRINT("Errore: impossibile creare il display framebuffer\n");
      return -1;
   }
   lv_linux_fbdev_set_file(dispp, DISP_DEV);

   // FULL render mode, double buffer
   lv_display_set_buffers(dispp,
                        fb_buf1,
                        fb_buf2,
                        buf_size_bytes,
                        LV_DISPLAY_RENDER_MODE_FULL);

   lv_linux_fbdev_set_force_refresh(dispp, false); //True per refreshare ad ogni cambiamento

   /* 5. Configura input touchscreen */
   lv_indev_t *touchIndev = lv_evdev_create(LV_INDEV_TYPE_POINTER, INPUT_DEV);
   if(!touchIndev) 
   {
      ERROR_PRINT("Errore: impossibile creare il dispositivo touch %s\n", INPUT_DEV);
      return -1;
   }

   int min_x, max_x, min_y, max_y;
   if(read_touch_range(INPUT_DEV, &min_x, &max_x, &min_y, &max_y) == 0) 
   {
      DEBUG_PRINT("Touch RAW range: X=%d..%d, Y=%d..%d\n", min_x, max_x, min_y, max_y);
      lv_evdev_set_swap_axes(touchIndev, false); 
      lv_evdev_set_calibration(touchIndev, min_x, min_y, max_x, max_y);
   } 
   else 
   {
      ERROR_PRINT("Impossibile leggere il range RAW del touch\n");
   }

   DEBUG_PRINT("Input device registrato: %s\n", INPUT_DEV);

   /* 6. Inizializzo interfaccia utente */
   ui_init();

   /* 7. Avvio thread GLib */
   pthread_t glib_thread;

   if (pthread_create(&glib_thread, NULL, glib_thread_func, NULL) != 0) 
   {
      ERROR_PRINT("Errore: impossibile creare thread GLib\n");
      return -1;
   }

   /* INIZIALIZZAZIONE TERMINATA */
   INFO_PRINT("--- LVGL + DISPLAY INIZIALIZZATI ---\n");

   /* 8. Ciclo principale LVGL */
   uint32_t last_tick = get_time_ms();
   while(1) 
   {
      uint32_t now = get_time_ms();
      lv_tick_inc(now - last_tick);
      last_tick = now;

      lv_timer_handler();  // gestisce GUI e animazioni

      usleep(LV_TICK_PERIOD * 1000); // 5 ms sleep per ridurre CPU
   }

   return 0;
}
