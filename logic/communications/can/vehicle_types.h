#ifndef VEHICLE_TYPES_H
#define VEHICLE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "../../logic.h"

/* =============================================
   STRUTTURE DATI
   ============================================= */

typedef struct btData_s {
    uint8_t connectionStatus;
    uint8_t deviceIdentified;
    uint8_t pairStatus;
    uint8_t macAddress[6];
    uint32_t passkey;
    uint8_t deviceUniqueId[16];
} btData_t;

typedef struct VMS_data_1_s {
    uint8_t Speed_DBS;              // km/h
    uint8_t Energy_Consumption;     // wh/km
    uint8_t SOC_dsb;                // %
    bool LowSOC;
    bool Ready_Lamp;
    bool Run_off_VMS;
    bool Side_Stand;
    uint8_t Modality_VMS_DSB;       // enum
    uint8_t counter_VMS_to_DSB;     // counter
    bool Flag_Limitation;
    bool Alarm_lamp;
    bool Flag_temp_lamp;
    bool Number_of_batteries;
    uint8_t Range_DSB;
} VMS_data_1_t;

typedef struct VMS_data_2_s {
    int8_t Power_battery_percentage;
    bool Buzzer_Status;
    bool MIL_lamp;
    bool key_off;
} VMS_data_2_t;

typedef struct DBS_data_1_s {
    uint8_t Modality_DSB_VMS;
    bool sharing_enable;
    bool Buzzer_request;
    uint8_t FrameCounterDSB;
    uint8_t Vehicle_Code;
    uint8_t Vehicle_Order;
} DBS_data_1_t;

typedef struct __attribute__((packed)) vehicle_s {
    VMS_data_1_t VMSData1;
    VMS_data_2_t VMSData2;
    DBS_data_1_t DBSData1;

    btData_t btInterface;

    uint16_t ambientTemperature;
    uint16_t ambientLight;
    uint8_t powerMap;
    uint8_t idMessage;
    uint8_t bleStatus;
    uint8_t chargingStatus;
    uint8_t hourTime;
    uint8_t minuteTime;
    bool high_beam;
    bool low_beam;
    bool turns;
    bool cruise_control;
    uint32_t tripDistance;
    uint32_t totalDistance;
    uint32_t error;
    uint32_t fwVersion;
    bool test_mode_flag;
    bool qr_code_flag;
} vehicle_t;

typedef struct my_struct_s {
    vehicle_t vehicle_struct;
    int canSoket;
    pthread_mutex_t lock; /* Mutex per proteggere vehicle struct */
} my_struct_t;

#endif /* VEHICLE_TYPES_H */