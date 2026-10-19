/*
 * Quectel EC800M-CN (sheet MODULE_SIM) tren USART2 PD5/PD6 115200.
 *
 * Moi chan dieu khien di qua transistor NPN nen DAO MUC:
 *   SIM_PWRKEY PD7 = 1 -> PWRKEY module bi keo thap (nhan nut nguon)
 *   SIM_RESET  PD4 = 1 -> RESET_N module bi keo thap (giu reset)
 *   SIM_STATUS PD3, SIM_NETSTATUS PD2: chi noi LED_Y2/Y3.
 * Thuc te tren board V1, PD3 doc ra 1 ca khi module dang chay, nen firmware
 * xac dinh module bat/tat bang cach hoi AT, khong dung chan STATUS.
 */
#ifndef EC800M_H
#define EC800M_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MDM_AT_OK = 0,
    MDM_AT_ERROR,
    MDM_AT_TIMEOUT,
} mdm_at_t;

bool mdm_is_on(void);                       /* module da tra loi AT */
bool mdm_power_on(void);                    /* hoi AT, chi bat PWRKEY neu im lang; doc IMEI, SIM */
void mdm_hard_reset(void);

/* Gui 1 lenh AT, gom cac dong tra loi (tru echo) vao resp den OK/ERROR */
mdm_at_t mdm_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

bool mdm_sim_ready(void);                   /* AT+CPIN? = READY o lan hoi cuoi */
void mdm_diag(void);                        /* in CFUN, QSIMDET, CSQ, COPS, QNWINFO... */
bool mdm_wait_network(uint32_t timeout_ms); /* cho dang ky mang LTE */
int  mdm_csq(void);                         /* 0..31, 99 = khong biet */
const char *mdm_imei(void);

bool mdm_mqtt_connect(const char *client_id, const char *sub_topic);
bool mdm_mqtt_connected(void);
bool mdm_mqtt_publish(const char *topic, const char *payload);
void mdm_mqtt_disconnect(void);

void mdm_sim_retry_now(void);               /* lan ket noi toi se tat/bat radio de nhan lai SIM */

/* Goi thoai toi so num, tu cup may sau max_ms neu ben kia chua cup */
bool mdm_call(const char *num, uint32_t max_ms);
bool mdm_sms_send(const char *num, const char *text);  /* chu khong dau (bang ma GSM) */

/* Goi thuong xuyen tu vong lap chinh de xu ly URC (+QMTRECV, +QMTSTAT) */
void mdm_poll(void);

/* Ham callback khi co ban tin MQTT toi (app dinh nghia lai) */
void mdm_on_mqtt_message(const char *topic, const char *payload);

/* Goi lien tuc khi driver dang cho module (co the mat hang chuc giay):
 * app dinh nghia lai de van xu ly dau vao/relay. KHONG goi lai ham mdm_* trong nay. */
void mdm_idle(void);

#endif
