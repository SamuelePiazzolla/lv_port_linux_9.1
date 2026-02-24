#ifndef LOGIC_H
#define LOGIC_H

//file .h in cui inserire tutte gli includi necessari per la logica inclusi i file per la gestione degli schermi

//********* GENERALI *********
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

//********* GESTIONE SCHERMI *********
#include "../lvgl/lvgl.h"

//********* GESTIONE UI *********
#include "../ui/ui.h"

// Gestione print
#define DEBUG 0     // gestisce output extra su console: 1 ON | 0 OFF 
#define INFO 1      // gestisce output di info importanti su console: 1 ON | 0 OFF
#define ERROR 1     // gestisce output di errori su console: 1 ON | 0 OFF

//*********** GESTIONE STAMPE *********** 
#if DEBUG
    #define DEBUG_PRINT(fmt, ...) g_print(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...)
#endif

#if INFO
    #define INFO_PRINT(fmt, ...) g_print(fmt, ##__VA_ARGS__)
#else
    #define INFO_PRINT(fmt, ...)
#endif

#if ERROR
    #define ERROR_PRINT(fmt, ...) g_printerr(fmt, ##__VA_ARGS__)
#else
    #define ERROR_PRINT(fmt, ...)
#endif

#endif