#ifndef AUDIO_LOGIC_H
#define AUDIO_LOGIC_H

/*
=====================================
    FUNZIONI
=====================================
*/
void logic_record_audio(void);          // Logica inizio record audio
void logic_stop_record_audio(void);     // Logica fine record audio
void logic_change_volume(void);         // Logica cambio volume
void logic_select_file_audio(void);     // Logica selezione file audio da riprodurre
void logic_play_file_audio(void);       // Logica play file audio
void logic_stop_file_audio(void);       // Logica stop file audio
void logic_reset_file_audio(void);      // Logica reset file audio
void logic_deinit_audio_screen(void);   // Logica deinit audio screen
void logic_init_audio_screen(void);     // Logica deinit audio screen


#endif