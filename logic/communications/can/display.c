#include <stdlib.h>   
#include <string.h>   
#include <time.h>
#include "display.h"


/* =======================================
    DEFINIZIONI
   ======================================= */

#define TRIP_CONVERTER 10

enum POWER_MAP {
    ECO = 0,
    COMFORT,
    TOURING,
    SPORT
};

/* =======================================
    PROTOTIPI
   ======================================= */

static void update_speed(uint8_t speed, displayParam_t * p);                // Aggiorna la label della velocità
static void update_battery(uint8_t battery, displayParam_t * p);            // Aggiorna la label e l'arco della batteria
static void update_energy_consumption(uint8_t energy, displayParam_t * p);  // Aggiorna la label e l'arco del consumo dell'energia
static void update_trip_label(uint32_t trip, displayParam_t * p);           // Aggiorna la label del trip
static void update_total_label(uint32_t total, displayParam_t * p);         // Aggiorna la label del total
static void update_powerMap(vehicle_t * v, displayParam_t * p);             // Aggiorna la label relativa alla powerMap (E / C / T / S)
static void update_time(displayParam_t * p);                                // Aggiorna la top label, relativa all'orario
void update_icons(vehicle_t * v, displayParam_t * p);                       // Mostra le icone richieste e nasconde le altre
void update_display_cb(void * user_data);                                   // Callback per aggiornare la UI in maniera thread-safe

/* =======================================
    IMPLEMENTAZIONI
   ======================================= */

/* ---------------------------------------
    FUNZIONI STATICHE DI AGGIORNAMENTO
   --------------------------------------- */

static void update_speed(uint8_t speed, displayParam_t * p)
{
    lv_label_set_text_fmt(p->speedNumberLabel, "%d", speed);
}

static void update_battery(uint8_t battery, displayParam_t * p)
{
    lv_arc_set_value(p->batteryArc, (uint16_t)battery);
 
    if (battery > 25)
        lv_obj_set_style_arc_color(p->batteryArc, lv_color_hex(0x00FF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    else if (battery > 10)
        lv_obj_set_style_arc_color(p->batteryArc, lv_color_hex(0xFFFF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    else
        lv_obj_set_style_arc_color(p->batteryArc, lv_color_hex(0xFF0000), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_label_set_text_fmt(p->batteryNumberLabel, "%d", battery);
}

static void update_energy_consumption(uint8_t energy, displayParam_t * p)
{
    lv_arc_set_value(p->efficiencyArc, (uint16_t)energy);

    if (energy < 50)
        lv_obj_set_style_arc_color(p->efficiencyArc, lv_color_hex(0x00FF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    else if (energy < 80)
        lv_obj_set_style_arc_color(p->efficiencyArc, lv_color_hex(0xFFFF00), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    else
        lv_obj_set_style_arc_color(p->efficiencyArc, lv_color_hex(0xFF0000), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_label_set_text_fmt(p->energyNumberLabel, "%d", energy);
}

static void update_trip_label(uint32_t trip, displayParam_t * p)
{
    lv_label_set_text_fmt(p->tripNumberLabel, "%u", trip / TRIP_CONVERTER);
}

static void update_total_label(uint32_t total, displayParam_t * p)
{
    lv_label_set_text_fmt(p->totalNumberLabel, "%u", total / TRIP_CONVERTER);
}

static void update_powerMap(vehicle_t * v, displayParam_t * p)
{
    if (v->DBSData1.Modality_DSB_VMS == v->VMSData1.Modality_VMS_DSB)
    {
        switch (v->VMSData1.Modality_VMS_DSB)   /* era: v->powerMap */
        {
        case ECO:
            lv_label_set_text(p->powerMapLabel, "E");
            lv_obj_set_style_text_color(p->powerMapLabel, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case COMFORT:
            lv_label_set_text(p->powerMapLabel, "C");
            lv_obj_set_style_text_color(p->powerMapLabel, lv_color_hex(0x0000FF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case TOURING:
            lv_label_set_text(p->powerMapLabel, "T");
            lv_obj_set_style_text_color(p->powerMapLabel, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case SPORT:
            lv_label_set_text(p->powerMapLabel, "S");
            lv_obj_set_style_text_color(p->powerMapLabel, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        default:
            break;
        }
    }
}

static void update_time(displayParam_t * p)
{
    time_t rawtime;
    struct tm * timeinfo;

    // Ottiene il timestamp attuale (secondi dal 1970)
    time(&rawtime);
    
    // Converte il timestamp in una struttura leggibile (ore, minuti, secondi)
    timeinfo = localtime(&rawtime);

    lv_label_set_text_fmt(p->timeLabel, "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
}

void update_icons(vehicle_t * v, displayParam_t * p)
{
    /* Abbaglianti */
    if (v->high_beam)
        lv_obj_remove_flag(p->highBeam, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(p->highBeam, LV_OBJ_FLAG_HIDDEN);

    /* Anabbaglianti */
    if (v->low_beam)
        lv_obj_remove_flag(p->lowBeam, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(p->lowBeam, LV_OBJ_FLAG_HIDDEN);

    /* Frecce direzionali (turns controlla entrambe) */
    if (v->turns)
    {
        lv_obj_remove_flag(p->turnLeft,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(p->turnRight, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p->turnLeft,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->turnRight, LV_OBJ_FLAG_HIDDEN);
    }

    /* Spia guasto powertrain */
    if (v->VMSData2.MIL_lamp)
        lv_obj_remove_flag(p->milLamp, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(p->milLamp, LV_OBJ_FLAG_HIDDEN);

    /* Cruise control */
    if (v->cruise_control)
        lv_obj_remove_flag(p->cruiseControl, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(p->cruiseControl, LV_OBJ_FLAG_HIDDEN);

    /* Limitazione velocità attiva */
    if (v->VMSData1.Flag_Limitation)
        lv_obj_remove_flag(p->flagLimitation, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(p->flagLimitation, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------
    CALLBACK ASYNC
   --------------------------------------- */

void update_display_cb(void * user_data)
{
    display_async_data_t * data = (display_async_data_t *)user_data;
    if (!data) return;

    displayParam_t * h = data->display;
    vehicle_t      * v = &data->vehicle;

    /* Schermata già distrutta: scartiamo l'aggiornamento senza crashare */
    if (!h || !h->mainScreen)
    {
        free(data);
        return;
    }

    update_speed(v->VMSData1.Speed_DBS, h);
    update_battery(v->VMSData1.SOC_dsb, h);
    update_energy_consumption(v->VMSData1.Energy_Consumption, h);
    update_trip_label(v->tripDistance, h);
    update_total_label(v->totalDistance, h);
    update_time(h);
    update_icons(v, h);
    update_powerMap(v, h);

    free(data);
}

/* ---------------------------------------
    API PUBBLICA
   --------------------------------------- */

bool update_display(vehicle_t * v, displayParam_t * h)
{
    if (!h || !h->mainScreen) return false;

    display_async_data_t * data = (display_async_data_t *)malloc(sizeof(display_async_data_t));
    if (!data) 
        return false;

    memcpy(&data->vehicle, v, sizeof(vehicle_t));
    data->display = h;

    lv_async_call(update_display_cb, data);
    return true;
}

/* -----------------------------------------------
    INIT / DEINIT
   ----------------------------------------------- */

void display_init(displayParam_t * p)
{
    p->mainScreen         = ui_canScreen;

    p->batteryArc         = ui_batteryArc;
    p->batteryNumberLabel = ui_batteryValue;

    p->efficiencyArc      = ui_energyArc;
    p->energyNumberLabel  = ui_energyValue;

    p->speedNumberLabel   = ui_speedLabel;

    p->timeLabel          = ui_topLabel;

    p->tripNumberLabel    = ui_trip;
    p->totalNumberLabel   = ui_total;

    p->powerMapLabel      = ui_shiftLabel;

    p->milLamp            = ui_milImg;
    p->highBeam           = ui_highBeamImg;
    p->lowBeam            = ui_lowBeamImg;
    p->turnLeft           = ui_turnLeftImg;
    p->turnRight          = ui_turnRightImg;
    p->cruiseControl      = ui_cruiseImg;
    p->flagLimitation     = ui_speedLimitImg;

    /* Nasconde tutte le icone allo stato iniziale*/
    lv_obj_add_flag(p->milLamp,        LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->highBeam,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lowBeam,        LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->turnLeft,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->turnRight,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->cruiseControl,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->flagLimitation, LV_OBJ_FLAG_HIDDEN);
}

void display_deinit(displayParam_t * p)
{
    memset(p, 0, sizeof(displayParam_t));
}