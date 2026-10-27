/*
 * Lam viec voi cac node LoRa (NODE_AVIET_V1):
 *   node -> gateway: JSON {"id":"349CE1DF","n":12,"up":70,...} (khong '\0' cuoi)
 *   gateway -> node: "@<id> <lenh> [tham so]" hoac "@* <lenh>" (moi node)
 * Goi nhan duoc cho vao hang doi (lora_rx_poll) - goi duoc ca khi 4G dang chan
 * (tu mdm_idle) vi SX1268 chi giu 1 goi. Xu ly / publish o lora_node_task() trong app_loop.
 */
#ifndef LORA_NODE_H
#define LORA_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void lora_node_init(void);

/* Chi doc goi tu SX1268 vao hang doi. KHONG goi ham mdm_* -> an toan trong mdm_idle(). */
void lora_rx_poll(void);

/* Lay goi khoi hang doi, cap nhat bang node, publish; kiem tra offline; dong bo gio node.
 * Chi goi tu app_loop (co the goi mdm_mqtt_publish). */
void lora_node_task(void);

/* Gui "@<id> <cmd>" (id = "*" cho moi node). Tu quay ve RX sau khi phat. */
bool lora_node_send(const char *id, const char *cmd);
/* Gui nguyen chuoi (lenh console "lora ...") */
bool lora_send_raw(const char *msg);

void lora_node_print(void);             /* lenh console "nodes" */
int  lora_node_online(void);            /* so node dang online */
int  lora_node_known(void);             /* so node trong bang */
uint32_t lora_rx_total(void);           /* tong so goi nhan (moi loai) */
int16_t  lora_last_rssi(void);

/* Doc truong chuoi muc 1 trong JSON: json_get_str("{\"node\":\"AB\"}", "node", ...) -> "AB" */
bool json_get_str(const char *json, const char *key, char *out, size_t n);

/* App dinh nghia: publish len <CFG_TOPIC_BASE>/<IMEI>/<suffix>. false neu MQTT chua ket noi. */
bool app_publish(const char *suffix, const char *payload);

#endif
