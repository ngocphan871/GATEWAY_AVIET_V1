/*
 * Cau hinh cua gateway - sua o day, khong can dong vao driver.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ===================== 4G EC800M + MQTT ===================== */
/* APN: Viettel "v-internet", Vinaphone "m3-world", Mobifone "m-wap" */
#define CFG_APN                 "v-internet"

#define CFG_MQTT_HOST           "broker.hivemq.com"
#define CFG_MQTT_PORT           1883
#define CFG_MQTT_USER           ""          /* de trong neu broker khong can */
#define CFG_MQTT_PASS           ""
/* Topic: <CFG_TOPIC_BASE>/<IMEI>/data | /lora | /cmd */
#define CFG_TOPIC_BASE          "aviet/gw"

#define CFG_PUBLISH_PERIOD_MS   30000u      /* chu ky gui du lieu len server */
#define CFG_RECONNECT_PERIOD_MS 30000u      /* thu ket noi lai sau moi ... */
/* Chua co SIM: tat/bat radio (CFUN 0/1) de nhan lai SIM. Moi lan nhu vay module
 * phai do lai song (CSQ = 99 mot luc) nen khong lam qua day. */
#define CFG_SIM_RETRY_MS        300000u

/* ===================== RS485 Modbus RTU ===================== */
#define CFG_MB_SLAVE            1           /* dia chi slave can doc */
#define CFG_MB_FUNC             3           /* 3 = holding, 4 = input register */
#define CFG_MB_START_REG        0
#define CFG_MB_REG_COUNT        10          /* toi da MB_MAX_REGS (32) */
#define CFG_MB_TIMEOUT_MS       300u
#define CFG_MB_POLL_PERIOD_MS   5000u

/* ===================== LoRa SX1262 (module S62F) ===================== */
#define CFG_LORA_FREQ_HZ        433000000u  /* doi theo bang tan module: 433/470/868/915/923 MHz */
#define CFG_LORA_SF             9           /* 5..12 */
#define CFG_LORA_BW             125         /* kHz: 125, 250, 500 */
#define CFG_LORA_CR             5           /* 4/5 .. 4/8 -> 5..8 */
#define CFG_LORA_TX_DBM         22          /* -9..22 */
#define CFG_LORA_PREAMBLE       8
#define CFG_LORA_SYNC_PUBLIC    0           /* 0 = private 0x1424, 1 = public/LoRaWAN 0x3444 */
/* Nap thu: TCXO=0 -> loi XOSC_START (0x0020), nen module S62F dung TCXO qua DIO3 */
#define CFG_LORA_USE_TCXO       1           /* 1 neu module dung TCXO cap boi DIO3 */
/* Chan dieu khien chuyen mach anten (RF_VCC1 = PC9, RF_VCC2 = PA10).
 * Chua co datasheet S62F nen day la doan: neu thu khong duoc thi doi cho 2 dong. */
#define CFG_LORA_TX_VC1         1
#define CFG_LORA_TX_VC2         0
#define CFG_LORA_RX_VC1         0
#define CFG_LORA_RX_VC2         1

/* ===================== Dau vao -> relay ===================== */
/* 1: IN1 dieu khien RELAY1, IN2 dieu khien RELAY2 (relay theo trang thai dau vao,
 * chi tac dong khi dau vao thay doi nen lenh MQTT van dung duoc giua 2 lan doi). */
#define CFG_INPUT_RELAY_LINK    1
#define CFG_INPUT_DEBOUNCE_MS   50u

/* ===================== Cam bien luu luong YF-S201C ===================== */
/* So xung tren 1 lit. YF-S201: F(Hz) = 7.5 * Q(L/phut) -> 450 xung/lit.
 * Can hieu chinh lai voi dong cam bien thuc te. */
#define CFG_FLOW_PULSES_PER_L   450u

/* ===================== Man hinh TFT (CN8) ===================== */
/* Chon 1 trong 3: TFT_ST7789 (240x320), TFT_ILI9341 (240x320), TFT_ST7735 (128x160) */
#define CFG_TFT_CONTROLLER      TFT_ST7789
#define CFG_TFT_ROTATION        1           /* 0..3 */

#endif
