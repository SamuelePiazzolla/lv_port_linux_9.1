#ifndef DISPLAY_H
#define DISPLAY_H

#include "vehicle_types.h"
#include "lvgl/lvgl.h"

LV_FONT_DECLARE(arcFont);
LV_FONT_DECLARE(labelFont);
LV_FONT_DECLARE(speedFont);
LV_FONT_DECLARE(timeFont);
LV_FONT_DECLARE(tripFont);
LV_FONT_DECLARE(unitFont);

/**
 * @brief Struttura che raccoglie i puntatori agli oggetti LVGL della schermata CAN
 */
typedef struct displayParam_s
{
    /* Schermata root */
    lv_obj_t * mainScreen;

    /* Arco batteria e relativi label */
    lv_obj_t * batteryArc;
    lv_obj_t * batteryNumberLabel;

    /* Arco efficienza energetica e relativi label */
    lv_obj_t * efficiencyArc;
    lv_obj_t * energyNumberLabel;

    /* Velocità */
    lv_obj_t * speedNumberLabel;

    /* Orario */
    lv_obj_t * timeLabel;

    /* Distanze */
    lv_obj_t * tripNumberLabel;
    lv_obj_t * totalNumberLabel;

    /* Modalità di guida (E / C / T / S) */
    lv_obj_t * powerMapLabel;

    /* Icone di stato — visibili/nascoste tramite LV_OBJ_FLAG_HIDDEN */
    lv_obj_t * milLamp;        /* ui_milImg        — spia guasto powertrain */
    lv_obj_t * highBeam;       /* ui_highBeamImg   — abbaglianti */
    lv_obj_t * lowBeam;        /* ui_lowBeamImg    — anabbaglianti */
    lv_obj_t * turnLeft;       /* ui_turnLeftImg   — freccia sinistra */
    lv_obj_t * turnRight;      /* ui_turnRightImg  — freccia destra */
    lv_obj_t * cruiseControl;  /* ui_cruiseImg     — cruise control */
    lv_obj_t * flagLimitation; /* ui_speedLimitImg — limite velocità attivo */

} displayParam_t;

/**
 * @brief Snapshot dei dati veicolo e della composizione del display passato come user_data alla lv_async_call.
 */
typedef struct display_async_data_s
{
    vehicle_t      vehicle;  /* Copia profonda dei dati veicolo al momento della richiesta */
    displayParam_t * display; /* Puntatore alla struct display (vive in canLogic.c)        */
} display_async_data_t;


/* ---------------------------------------------------------------
   Funzioni pubbliche
   --------------------------------------------------------------- */

/**
 * @brief  Inizializza displayParam_t collegando i puntatori agli oggetti LVGL
 */
void display_init(displayParam_t * p);

/**
 * @brief  Azzera tutti i puntatori di displayParam_t (memset a 0).
 */
void display_deinit(displayParam_t * p);

/**
 * @brief  Accoda un aggiornamento display tramite lv_async_call.
 *
 * @param  v  Puntatore alla struttura veicolo corrente (lettura)
 * @param  h  Puntatore alla struttura display
 * @return true se l'accodamento è riuscito,
 *         false se h/mainScreen è NULL o se malloc fallisce.
 */
bool update_display(vehicle_t * v, displayParam_t * h);

/**
 * @brief  Callback eseguita nel thread UI da lv_timer_handler().
 * 
 *         Aggiorna tutti i widget LVGL con i dati dello snapshot,
 *         poi libera lo snapshot con free().
 *         Se mainScreen è NULL (schermata già distrutta) scarta
 *         silenziosamente l'aggiornamento e libera comunque la memoria.
 */
void update_display_cb(void * user_data);

/**
 * @brief  Aggiorna le sole icone di stato (abbaglianti, frecce, MIL, ecc.)
 */
void update_icons(vehicle_t * v, displayParam_t * p);


#endif /* DISPLAY_H */