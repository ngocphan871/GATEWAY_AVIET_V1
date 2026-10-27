#include "lora_node.h"
#include "clock.h"
#include "config.h"
#include "sx1262.h"
#include "main.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool     used;
    bool     online;
    bool     st_pending;     /* can publish {"id":..,"online":x} (thu lai neu MQTT chua san) */
    bool     time_sent;      /* da gui gio cho node it nhat 1 lan */
    bool     time_pending;   /* can gui gio ngay (node moi khoi dong lai) */
    char     id[9];
    int32_t  last_n;         /* -1 = chua co */
    uint32_t last_up;
    uint32_t rx;
    uint32_t lost;
    int16_t  rssi;
    int8_t   snr;
    uint32_t t_last;         /* HAL_GetTick luc nghe goi cuoi */
    uint32_t t_time;         /* luc gui gio lan cuoi */
} node_t;

static node_t nodes[CFG_NODE_MAX];

/* Hang doi vong: giu duoc CFG_LORA_RXQ_LEN - 1 goi */
static sx1262_packet_t rxq[CFG_LORA_RXQ_LEN];
static volatile uint8_t q_head, q_tail;
static uint32_t q_drop;

static uint32_t rx_total;
static int16_t rssi_last;
static uint32_t t_last_tx;

/* ---------------- JSON don gian (chi doc truong muc 1) ---------------- */

static const char *json_find(const char *json, const char *key)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) { return NULL; }
    p += strlen(pat);
    while (*p == ' ') { p++; }
    return p;
}

bool json_get_str(const char *json, const char *key, char *out, size_t n)
{
    const char *p = json_find(json, key);
    if (!p || *p != '"' || n == 0) { return false; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) { out[i++] = *p++; }
    out[i] = '\0';
    return *p == '"';
}

static bool json_get_int(const char *json, const char *key, long *v)
{
    const char *p = json_find(json, key);
    if (!p || !(isdigit((unsigned char)*p) || *p == '-')) { return false; }
    *v = strtol(p, NULL, 10);
    return true;
}

/* ---------------- nhan ---------------- */

void lora_node_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    q_head = q_tail = 0;
}

void lora_rx_poll(void)
{
    static sx1262_packet_t spare;
    uint8_t next = (uint8_t)((q_head + 1u) % CFG_LORA_RXQ_LEN);
    sx1262_packet_t *p = (next != q_tail) ? &rxq[q_head] : &spare;

    if (!sx1262_poll_rx(p)) { return; }
    rx_total++;
    rssi_last = p->rssi;
    if (p == &spare) {          /* hang doi day: van phai doc chip, bo goi moi */
        q_drop++;
        return;
    }
    q_head = next;
}

uint32_t lora_rx_total(void) { return rx_total; }
int16_t lora_last_rssi(void) { return rssi_last; }

static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 8) { return false; }
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)id[i])) { return false; }
    }
    return true;
}

static node_t *find_node(const char *id)
{
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        if (nodes[i].used && strcasecmp(nodes[i].id, id) == 0) { return &nodes[i]; }
    }
    return NULL;
}

/* O trong, neu het thi lay node offline lau nhat */
static node_t *alloc_node(void)
{
    node_t *old = NULL;
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        if (!nodes[i].used) { return &nodes[i]; }
        if (!nodes[i].online && (old == NULL || nodes[i].t_last < old->t_last)) { old = &nodes[i]; }
    }
    return old;
}

static void publish_status(node_t *nd)
{
    char js[48];
    snprintf(js, sizeof(js), "{\"id\":\"%s\",\"online\":%d}", nd->id, nd->online ? 1 : 0);
    char sfx[24];
    snprintf(sfx, sizeof(sfx), "node/%s", nd->id);
    if (app_publish(sfx, js)) { nd->st_pending = false; }
}

static void node_update(const char *id, const char *msg, const sx1262_packet_t *p)
{
    node_t *nd = find_node(id);
    bool is_new = false;
    long n = 0, up = 0;
    bool has_n = json_get_int(msg, "n", &n);
    bool has_up = json_get_int(msg, "up", &up);

    if (nd == NULL) {
        nd = alloc_node();
        if (nd == NULL) {
            printf("[NODE %s] bang node day (%d), khong theo doi\n", id, CFG_NODE_MAX);
            return;
        }
        memset(nd, 0, sizeof(*nd));
        nd->used = true;
        nd->last_n = -1;
        for (int i = 0; i < 8 && id[i]; i++) { nd->id[i] = (char)toupper((unsigned char)id[i]); }
        is_new = true;
        printf("[NODE %s] node moi\n", nd->id);
    }

    if (has_n && nd->last_n >= 0) {
        uint16_t d = (uint16_t)((uint16_t)n - (uint16_t)nd->last_n);
        if (has_up && (uint32_t)up < nd->last_up) {
            printf("[NODE %s] node khoi dong lai (up %lu -> %ld)\n", nd->id,
                   (unsigned long)nd->last_up, up);
            nd->time_pending = true;
        } else if (d == 0) {
            printf("[NODE %s] goi trung n=%ld\n", nd->id, n);
        } else if (d > 1u && d < 1000u) {
            nd->lost += d - 1u;
            printf("[NODE %s] mat %u goi (n %ld -> %ld)\n", nd->id, d - 1u, (long)nd->last_n, n);
        }
    }
    if (has_n) { nd->last_n = (int32_t)(n & 0xFFFF); }
    if (has_up) { nd->last_up = (uint32_t)up; }

    nd->rx++;
    nd->rssi = p->rssi;
    nd->snr = p->snr;
    nd->t_last = HAL_GetTick();
    if (!nd->online) {
        nd->online = true;
        if (!is_new) {
            printf("[NODE %s] online tro lai\n", nd->id);
            nd->st_pending = true;
        }
    }
}

static void handle_packet(const sx1262_packet_t *p)
{
    char msg[SX1262_MAX_PAYLOAD + 1];
    char id[12];

    memcpy(msg, p->data, p->len);
    msg[p->len] = '\0';

    /* Goi bao cao cua node: {"id":"XXXXXXXX",...} */
    if (p->len >= 2 && msg[0] == '{' && msg[p->len - 1] == '}' &&
        json_get_str(msg, "id", id, sizeof(id)) && valid_id(id)) {
        node_update(id, msg, p);

        /* Them do manh tin hieu gateway do duoc truoc dau '}' cuoi */
        char out[SX1262_MAX_PAYLOAD + 48];
        snprintf(out, sizeof(out), "%.*s,\"rssi\":%d,\"snr\":%d}", p->len - 1, msg, p->rssi, p->snr);
        printf("[NODE %s] RSSI %d SNR %d: %s\n", id, p->rssi, p->snr, msg);

        char sfx[24];
        node_t *nd = find_node(id);
        snprintf(sfx, sizeof(sfx), "node/%s", nd ? nd->id : id);
        app_publish(sfx, out);
        return;
    }

    if (msg[0] == '@') {        /* lenh cua 1 gateway khac */
        printf("[LORA] bo qua lenh (gateway khac?): %s\n", msg);
        return;
    }

    /* Goi la: chuyen tiep dang hex */
    char js[2 * SX1262_MAX_PAYLOAD + 64];
    int n = snprintf(js, sizeof(js), "{\"rssi\":%d,\"snr\":%d,\"hex\":\"", p->rssi, p->snr);
    for (int i = 0; i < p->len; i++) {
        n += snprintf(js + n, sizeof(js) - (size_t)n, "%02X", p->data[i]);
    }
    snprintf(js + n, sizeof(js) - (size_t)n, "\"}");
    printf("[LORA] goi la %u byte, RSSI %d SNR %d: %s\n", p->len, p->rssi, p->snr, js);
    app_publish("lora", js);
}

/* ---------------- gui ---------------- */

bool lora_send_raw(const char *msg)
{
    size_t len = strlen(msg);
    if (len == 0 || len > SX1262_MAX_PAYLOAD) { return false; }
    lora_rx_poll();             /* cat goi dang nam trong chip truoc khi phat */
    bool ok = sx1262_send((const uint8_t *)msg, (uint8_t)len, 3000);
    t_last_tx = HAL_GetTick();
    printf("[LORA] gui \"%s\": %s\n", msg, ok ? "OK" : "LOI");
    return ok;
}

bool lora_node_send(const char *id, const char *cmd)
{
    char msg[SX1262_MAX_PAYLOAD + 1];
    int n = snprintf(msg, sizeof(msg), "@%s %s", id, cmd);
    if (n <= 0 || n >= (int)sizeof(msg)) { return false; }
    return lora_send_raw(msg);
}

/* ---------------- dinh ky ---------------- */

static void check_offline(void)
{
    uint32_t now = HAL_GetTick();
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        node_t *nd = &nodes[i];
        if (!nd->used) { continue; }
        if (nd->online && (now - nd->t_last) > CFG_NODE_OFFLINE_MS) {
            nd->online = false;
            nd->st_pending = true;
            printf("[NODE %s] OFFLINE (%lu s khong nghe)\n", nd->id,
                   (unsigned long)((now - nd->t_last) / 1000u));
        }
        if (nd->st_pending) { publish_status(nd); }
    }
}

/* Gui gio cho node: node moi, node vua khoi dong lai, va moi CFG_NODE_TIME_SYNC_MS */
static void time_sync_nodes(void)
{
    uint32_t now = HAL_GetTick();
    if (!clk_valid() || (now - t_last_tx) < 2000u) { return; }

    for (int i = 0; i < CFG_NODE_MAX; i++) {
        node_t *nd = &nodes[i];
        if (!nd->used || !nd->online) { continue; }
        bool due = nd->time_pending || !nd->time_sent ||
                   (now - nd->t_time) >= CFG_NODE_TIME_SYNC_MS;
        /* Node vua phat xong can vai tram ms de quay ve thu */
        if (!due || (now - nd->t_last) < 300u) { continue; }

        char ts[24], cmd[32];
        clk_format(ts, sizeof(ts));
        snprintf(cmd, sizeof(cmd), "time %s", ts);
        printf("[NODE %s] dong bo gio\n", nd->id);
        lora_node_send(nd->id, cmd);
        nd->time_pending = false;
        nd->time_sent = true;
        nd->t_time = HAL_GetTick();
        return;                 /* moi lan chi 1 node de cac goi tra loi khong dung nhau */
    }
}

void lora_node_task(void)
{
    static sx1262_packet_t pkt;

    lora_rx_poll();
    while (q_tail != q_head) {
        pkt = rxq[q_tail];      /* chep ra truoc: publish ben duoi co the goi lai lora_rx_poll */
        q_tail = (uint8_t)((q_tail + 1u) % CFG_LORA_RXQ_LEN);
        handle_packet(&pkt);
    }
    check_offline();
    time_sync_nodes();
}

/* ---------------- thong tin ---------------- */

int lora_node_online(void)
{
    int n = 0;
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        if (nodes[i].used && nodes[i].online) { n++; }
    }
    return n;
}

int lora_node_known(void)
{
    int n = 0;
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        if (nodes[i].used) { n++; }
    }
    return n;
}

void lora_node_print(void)
{
    uint32_t now = HAL_GetTick();
    printf("--- Node LoRa: %d online / %d ---\n", lora_node_online(), lora_node_known());
    printf("ID        TT   nhan  mat(%%)    RSSI SNR  n      nghe cach\n");
    for (int i = 0; i < CFG_NODE_MAX; i++) {
        node_t *nd = &nodes[i];
        if (!nd->used) { continue; }
        uint32_t tot = nd->rx + nd->lost;
        printf("%-8s  %-4s %5lu %4lu(%2lu%%) %5d %3d  %-5ld  %lu s\n", nd->id,
               nd->online ? "ON" : "OFF", (unsigned long)nd->rx, (unsigned long)nd->lost,
               (unsigned long)(tot ? nd->lost * 100u / tot : 0u), nd->rssi, nd->snr,
               (long)nd->last_n, (unsigned long)((now - nd->t_last) / 1000u));
    }
    printf("Tong goi nhan %lu, bo vi hang doi day %lu\n", (unsigned long)rx_total, (unsigned long)q_drop);
}
