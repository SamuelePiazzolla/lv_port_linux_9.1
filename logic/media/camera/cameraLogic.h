#ifndef CAMERA_LOGIC_H
#define CAMERA_LOGIC_H

/*
=====================================
    FUNZIONI
=====================================
*/
void logic_load_video(void);            // Logica pulsante load video
void logic_load_camera(void);           // Logica pulsante load camera
void logic_start_video(void);           // Logica pulsante start
void logic_stop_video(void);            // Logica pulsante stop
void logic_reset_video(void);           // Logica pulsante reset
void logic_rec_video(void);             // Logica pulsante rec
void logic_stop_rec_video(void);        // Logica pulsante stop rec
void createImgDisplayer(void);          // Inizializzazione schermo
void logic_deinit_camera_screen(void);  // Logica uscita dallo schermo 

#endif