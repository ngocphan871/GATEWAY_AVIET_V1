#include "ec800m.h"
#include "config.h"
#include "serial.h"
#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 512

static char line[LINE_MAX];
static uint16_t line_len;
static char imei[20];
static bool mqtt_up;
static bool alive;              /* module tra loi AT */
static bool sim_ready;          /* AT+CPIN? = READY */
static bool sim_checked;        /* da hoi AT+CPIN? it nhat 1 lan */
static uint32_t t_sim_retry;    /* lan cuoi tat/bat radio de nhan lai SIM */
char sim_state[48];             /* tra loi AT+CPIN? cuoi cung (doc duoc qua SWD) */

/* ---------------------------------------------------------------- */

bool mdm_is_on(void)
{
    return alive;
}

/* Goi lien tuc trong luc driver dang cho module (app dung de xu ly dau vao) */
__attribute__((weak)) void mdm_idle(void)
{
}

static void wait_ms(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < ms) { mdm_idle(); }
}

static void send(const char *s)
{
    ser_puts(SER_SIM, s);
}

__attribute__((weak)) void mdm_on_mqtt_message(const char *topic, const char *payload)
{
    (void)topic;
    (void)payload;
}

/* +QMTRECV: 0,0,"topic","payload" */
static void parse_qmtrecv(char *l)
{
    char *t0 = strchr(l, '"');
    if (!t0) { return; }
    char *t1 = strchr(t0 + 1, '"');
    if (!t1) { return; }
    *t1 = '\0';
    char *p0 = strchr(t1 + 1, '"');
    char *p1 = strrchr(t1 + 1, '"');
    if (!p0 || p1 <= p0) { return; }
    *p1 = '\0';
    mdm_on_mqtt_message(t0 + 1, p0 + 1);
}

/* Tra ve true neu dong la URC va da xu ly */
static bool handle_urc(char *l)
{
    if (strncmp(l, "+QMTRECV:", 9) == 0) {
        parse_qmtrecv(l);
        return true;
    }
    if (strncmp(l, "+QMTSTAT:", 9) == 0) {
        printf("[4G] MQTT mat ket noi: %s\n", l);
        mqtt_up = false;
        return true;
    }
    if (strcmp(l, "RDY") == 0 || strncmp(l, "+CPIN:", 6) == 0 ||
        strncmp(l, "+QIND:", 6) == 0 || strncmp(l, "+QIURC:", 7) == 0) {
        return false;   /* de cho lenh dang cho doc neu can */
    }
    return false;
}

/* Doc 1 dong (khong rong). true neu co dong trong thoi gian cho. */
static bool read_line(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        int c = ser_getc(SER_SIM);
        if (c < 0) {
            if ((HAL_GetTick() - t0) >= timeout_ms) { return false; }
            mdm_idle();
            continue;
        }
        if (c == '\r') { continue; }
        if (c == '\n') {
            if (line_len == 0) { continue; }
            line[line_len] = '\0';
            line_len = 0;
            return true;
        }
        if (line_len < LINE_MAX - 1) { line[line_len++] = (char)c; }
    }
}

void mdm_poll(void)
{
    while (read_line(0)) {
        if (!handle_urc(line)) {
            /* dong khong ai cho: in ra de debug */
            printf("[4G] %s\n", line);
        }
    }
}

mdm_at_t mdm_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    size_t used = 0;
    if (resp && resp_len) { resp[0] = '\0'; }

    mdm_poll();                         /* xu ly URC con ton truoc khi gui */
    send(cmd);
    send("\r\n");

    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint32_t el = HAL_GetTick() - t0;
        if (el >= timeout_ms || !read_line(timeout_ms - el)) { return MDM_AT_TIMEOUT; }
        if (handle_urc(line)) { continue; }
        if (strcmp(line, cmd) == 0) { continue; }           /* echo */
        if (strcmp(line, "OK") == 0) { return MDM_AT_OK; }
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0 ||
            strncmp(line, "+CMS ERROR", 10) == 0) {
            if (resp && used + strlen(line) + 2 < resp_len) { strcpy(resp + used, line); }
            return MDM_AT_ERROR;
        }
        if (resp && used + strlen(line) + 2 < resp_len) {
            used += (size_t)sprintf(resp + used, "%s\n", line);
        }
    }
}

/* Cho 1 URC bat dau bang prefix, chep phan sau prefix vao out */
static bool wait_urc(const char *prefix, char *out, size_t out_len, uint32_t timeout_ms)
{
    size_t pl = strlen(prefix);
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint32_t el = HAL_GetTick() - t0;
        if (el >= timeout_ms || !read_line(timeout_ms - el)) { return false; }
        if (strncmp(line, prefix, pl) == 0) {
            if (out && out_len) {
                strncpy(out, line + pl, out_len - 1);
                out[out_len - 1] = '\0';
            }
            return true;
        }
        if (strcmp(line, "OK") == 0) { continue; }
        if (strstr(line, "ERROR")) {
            printf("[4G] %s\n", line);
            return false;
        }
        if (!handle_urc(line)) { printf("[4G] %s\n", line); }
    }
}

void mdm_hard_reset(void)
{
    HAL_GPIO_WritePin(SIM_RESET_GPIO_Port, SIM_RESET_Pin, GPIO_PIN_SET);     /* RESET_N thap */
    wait_ms(300);
    HAL_GPIO_WritePin(SIM_RESET_GPIO_Port, SIM_RESET_Pin, GPIO_PIN_RESET);
    mqtt_up = false;
    alive = false;
}

bool mdm_power_on(void)
{
    char r[96];

    HAL_GPIO_WritePin(SIM_RESET_GPIO_Port, SIM_RESET_Pin, GPIO_PIN_RESET);   /* nha reset */
    mqtt_up = false;

    /*
     * Khong dua vao chan STATUS: tren board V1 PD3 doc ra 1 ca khi module
     * dang chay. Xung PWRKEY khi module dang bat se TAT no, nen hoi AT truoc.
     */
    alive = false;
    for (int i = 0; i < 3 && !alive; i++) {
        alive = (mdm_at("AT", NULL, 0, 300) == MDM_AT_OK);
    }
    if (!alive) {
        printf("[4G] Bat nguon module (PWRKEY 800 ms)...\n");
        HAL_GPIO_WritePin(SIM_PWRKEY_GPIO_Port, SIM_PWRKEY_Pin, GPIO_PIN_SET);
        wait_ms(800);
        HAL_GPIO_WritePin(SIM_PWRKEY_GPIO_Port, SIM_PWRKEY_Pin, GPIO_PIN_RESET);

        /* Main UART san sang sau ~5-10 s: go AT den khi OK */
        uint32_t t0 = HAL_GetTick();
        while (!alive && (HAL_GetTick() - t0) < 20000) {
            alive = (mdm_at("AT", NULL, 0, 500) == MDM_AT_OK);
        }
        if (!alive) {
            printf("[4G] Module khong tra loi AT\n");
            return false;
        }
    }
    mdm_at("ATE0", NULL, 0, 1000);
    mdm_at("AT+CMEE=2", NULL, 0, 1000);

    imei[0] = '\0';
    if (mdm_at("AT+CGSN", r, sizeof(r), 2000) == MDM_AT_OK) {
        sscanf(r, "%19[0-9]", imei);
    }

    /* QSIMDET dang tat nen module khong tu thay SIM cam vao luc dang chay:
     * lan truoc chua co SIM thi khoi dong lai giao tiep SIM bang CFUN 0/1 */
    if (sim_checked && !sim_ready && (HAL_GetTick() - t_sim_retry) >= CFG_SIM_RETRY_MS) {
        printf("[4G] Thu nhan lai SIM (CFUN 0/1)...\n");
        mdm_at("AT+CFUN=0", NULL, 0, 15000);
        mdm_at("AT+CFUN=1", NULL, 0, 15000);
        wait_ms(2000);
        t_sim_retry = HAL_GetTick();
    }
    if (!sim_checked) { t_sim_retry = HAL_GetTick(); }
    bool first = !sim_checked;
    sim_checked = true;
    sim_ready = false;
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 10000) {
        mdm_at("AT+CPIN?", r, sizeof(r), 2000);
        snprintf(sim_state, sizeof(sim_state), "%.47s", r);
        if (strstr(r, "READY")) {
            sim_ready = true;
            break;
        }
        wait_ms(500);
    }
    printf("[4G] Module OK, IMEI %s, SIM: %s\n", imei, sim_ready ? "san sang" : sim_state);
    if (first || sim_ready) { mdm_diag(); }
    return true;     /* module van chay, ke ca khi chua co SIM */
}

/* In ra cac thong tin chan doan SIM / song / mang */
void mdm_diag(void)
{
    static const char *const cmds[] = {
        "AT+CFUN?", "AT+QSIMDET?", "AT+QSIMSTAT?", "AT+QCCID", "AT+CSQ",
        "AT+COPS?", "AT+QNWINFO", "AT+CEREG?",
    };
    char r[160];
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        mdm_at_t st = mdm_at(cmds[i], r, sizeof(r), 3000);
        for (char *p = r; *p; p++) { if (*p == '\n') { *p = ' '; } }
        printf("[4G] %s -> %s%s\n", cmds[i], r,
               st == MDM_AT_OK ? "OK" : st == MDM_AT_ERROR ? "" : "[timeout]");
    }
}

bool mdm_sim_ready(void)
{
    return sim_ready;
}

const char *mdm_imei(void)
{
    return imei;
}

int mdm_csq(void)
{
    char r[64];
    int rssi = 99, ber = 99;
    if (mdm_at("AT+CSQ", r, sizeof(r), 1000) == MDM_AT_OK) {
        char *p = strstr(r, "+CSQ:");
        if (p) { sscanf(p + 5, "%d,%d", &rssi, &ber); }
    }
    return rssi;
}

static bool registered(const char *cmd, const char *tag)
{
    char r[64];
    int n = 0, stat = 0;
    if (mdm_at(cmd, r, sizeof(r), 1000) != MDM_AT_OK) { return false; }
    char *p = strstr(r, tag);
    if (!p || sscanf(p + strlen(tag), "%d,%d", &n, &stat) != 2) { return false; }
    return stat == 1 || stat == 5;    /* 1 = mang nha, 5 = roaming */
}

bool mdm_wait_network(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint32_t t_print = 0;
    while ((HAL_GetTick() - t0) < timeout_ms) {
        if (registered("AT+CEREG?", "+CEREG:") || registered("AT+CREG?", "+CREG:")) {
            return true;
        }
        if (HAL_GetTick() - t_print >= 10000u) {
            t_print = HAL_GetTick();
            printf("[4G] Dang tim mang... CSQ %d\n", mdm_csq());
        }
        wait_ms(1000);
    }
    return false;
}

bool mdm_mqtt_connected(void)
{
    return mqtt_up;
}

void mdm_mqtt_disconnect(void)
{
    mdm_at("AT+QMTDISC=0", NULL, 0, 5000);
    mqtt_up = false;
}

bool mdm_mqtt_connect(const char *client_id, const char *sub_topic)
{
    char cmd[200], r[128];
    int idx = -1, res = -1, code = -1;

    mqtt_up = false;
    if (!sim_ready) {
        printf("[4G] Khong co SIM -> khong the vao mang\n");
        return false;
    }
    if (!mdm_wait_network(60000)) {
        printf("[4G] Chua dang ky duoc mang\n");
        return false;
    }

    /* PDP context */
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", CFG_APN);
    mdm_at(cmd, NULL, 0, 2000);
    if (mdm_at("AT+QIACT?", r, sizeof(r), 5000) != MDM_AT_OK || !strstr(r, "+QIACT: 1,1")) {
        if (mdm_at("AT+QIACT=1", r, sizeof(r), 60000) != MDM_AT_OK) {
            printf("[4G] Khong mo duoc PDP (APN %s): %s\n", CFG_APN, r);
            return false;
        }
    }

    /* Ban tin toi hien thang trong URC +QMTRECV */
    mdm_at("AT+QMTCFG=\"recv/mode\",0,0,1", NULL, 0, 2000);
    mdm_at("AT+QMTCLOSE=0", NULL, 0, 5000);     /* dong phien cu neu con */

    snprintf(cmd, sizeof(cmd), "AT+QMTOPEN=0,\"%s\",%d", CFG_MQTT_HOST, CFG_MQTT_PORT);
    if (mdm_at(cmd, r, sizeof(r), 5000) != MDM_AT_OK ||
        !wait_urc("+QMTOPEN:", r, sizeof(r), 75000) ||
        sscanf(r, "%d,%d", &idx, &res) != 2 || res != 0) {
        printf("[4G] QMTOPEN loi (%d) %s:%d\n", res, CFG_MQTT_HOST, CFG_MQTT_PORT);
        return false;
    }

    if (CFG_MQTT_USER[0]) {
        snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"%s\",\"%s\",\"%s\"",
                 client_id, CFG_MQTT_USER, CFG_MQTT_PASS);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"%s\"", client_id);
    }
    if (mdm_at(cmd, r, sizeof(r), 5000) != MDM_AT_OK ||
        !wait_urc("+QMTCONN:", r, sizeof(r), 15000) ||
        sscanf(r, "%d,%d,%d", &idx, &res, &code) < 2 || res != 0 || code > 0) {
        printf("[4G] QMTCONN loi res=%d code=%d\n", res, code);
        return false;
    }
    mqtt_up = true;

    if (sub_topic && sub_topic[0]) {
        snprintf(cmd, sizeof(cmd), "AT+QMTSUB=0,1,\"%s\",1", sub_topic);
        if (mdm_at(cmd, r, sizeof(r), 5000) != MDM_AT_OK ||
            !wait_urc("+QMTSUB:", r, sizeof(r), 15000)) {
            printf("[4G] Subscribe %s loi\n", sub_topic);
        }
    }
    printf("[4G] MQTT da ket noi %s:%d\n", CFG_MQTT_HOST, CFG_MQTT_PORT);
    return true;
}

/* Cho dau nhac '>' (khong co xuong dong) sau AT+QMTPUBEX / AT+CMGS */
static bool wait_prompt(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    bool prompt = false;
    while ((HAL_GetTick() - t0) < timeout_ms) {
        int c = ser_getc(SER_SIM);
        if (c < 0) { mdm_idle(); continue; }
        if (c == '>') { prompt = true; break; }
        if (c == '\n' && line_len) {
            line[line_len] = '\0';
            line_len = 0;
            if (strstr(line, "ERROR")) {
                printf("[4G] %s\n", line);
                break;
            }
            handle_urc(line);
        } else if (c != '\r' && c != '\n' && line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
        }
    }
    line_len = 0;
    return prompt;
}

bool mdm_mqtt_publish(const char *topic, const char *payload)
{
    char cmd[160], r[64];
    int idx = -1, msgid = -1, res = -1;
    size_t len = strlen(payload);

    if (!mqtt_up) { return false; }
    mdm_poll();
    snprintf(cmd, sizeof(cmd), "AT+QMTPUBEX=0,0,0,0,\"%s\",%u", topic, (unsigned)len);
    send(cmd);
    send("\r\n");
    if (!wait_prompt(5000)) {
        printf("[4G] Publish: khong co dau nhac '>'\n");
        return false;
    }
    ser_write(SER_SIM, payload, len);

    if (!wait_urc("+QMTPUBEX:", r, sizeof(r), 15000) ||
        sscanf(r, "%d,%d,%d", &idx, &msgid, &res) != 3 || res != 0) {
        printf("[4G] Publish loi (%d)\n", res);
        return false;
    }
    return true;
}

/*
 * Gio dia phuong tu mang (NITZ). AT+QLTS=2 -> +QLTS: "2026/10/19,11:30:45+28,0"
 * Mang chua gui gio thi tra ve chuoi rong -> thu AT+CCLK? ("26/10/19,11:30:45+28").
 */
static bool parse_net_time(const char *r, const char *tag, clk_time_t *t)
{
    int y, mo, d, h, mi, s;
    const char *p = strstr(r, tag);
    if (!p || (p = strchr(p, '"')) == NULL) { return false; }
    if (sscanf(p + 1, "%d/%d/%d,%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) { return false; }
    if (y < 100) { y += 2000; }
    if (y < 2024 || y > 2099) { return false; }      /* chua dong bo: thuong ra 1980/2000 */
    t->year = (uint16_t)y;
    t->mon = (uint8_t)mo;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)h;
    t->min = (uint8_t)mi;
    t->sec = (uint8_t)s;
    return true;
}

bool mdm_get_time(clk_time_t *t)
{
    char r[96];
    if (!alive) { return false; }
    if (mdm_at("AT+QLTS=2", r, sizeof(r), 3000) == MDM_AT_OK && parse_net_time(r, "+QLTS:", t)) {
        return true;
    }
    if (mdm_at("AT+CCLK?", r, sizeof(r), 3000) == MDM_AT_OK && parse_net_time(r, "+CCLK:", t)) {
        return true;
    }
    return false;
}

void mdm_sim_retry_now(void)
{
    t_sim_retry = HAL_GetTick() - CFG_SIM_RETRY_MS;
}

/* ---------------- goi dien / SMS ---------------- */

bool mdm_call(const char *num, uint32_t max_ms)
{
    char cmd[40], r[128];

    if (!sim_ready) {
        printf("[4G] Chua co SIM -> khong goi duoc\n");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "ATD%s;", num);       /* dau ; = cuoc goi thoai */
    printf("[4G] Goi %s ...\n", num);
    if (mdm_at(cmd, r, sizeof(r), 10000) != MDM_AT_OK) {
        printf("[4G] ATD loi: %s\n", r);
        return false;
    }

    /* Theo doi cuoc goi: URC ket thuc, va CLCC moi 5 s
     * (stat: 0 dang noi, 2 dang quay so, 3 dang do chuong) */
    uint32_t t0 = HAL_GetTick(), t_clcc = t0;
    bool ended = false;
    while (!ended && (HAL_GetTick() - t0) < max_ms) {
        if (HAL_GetTick() - t_clcc >= 5000u) {
            t_clcc = HAL_GetTick();
            mdm_at("AT+CLCC", r, sizeof(r), 2000);
            for (char *p = r; *p; p++) { if (*p == '\n') { *p = ' '; } }
            printf("[4G] CLCC: %s\n", r[0] ? r : "(khong co cuoc goi)");
            if (!strstr(r, "+CLCC:")) { ended = true; }
            continue;
        }
        if (!read_line(200)) { continue; }
        if (handle_urc(line)) { continue; }
        printf("[4G] %s\n", line);
        if (strstr(line, "NO CARRIER") || strstr(line, "BUSY") ||
            strstr(line, "NO ANSWER") || strstr(line, "NO DIALTONE")) {
            ended = true;
        }
    }
    if (!ended) {
        mdm_at("ATH", NULL, 0, 5000);
        printf("[4G] Het %lu s, da cup may\n", (unsigned long)(max_ms / 1000u));
    } else {
        printf("[4G] Cuoc goi ket thuc\n");
    }
    return true;
}

bool mdm_sms_send(const char *num, const char *text)
{
    char cmd[48], r[64];

    if (!sim_ready) {
        printf("[4G] Chua co SIM -> khong nhan tin duoc\n");
        return false;
    }
    mdm_at("AT+CMGF=1", NULL, 0, 2000);              /* che do text */
    mdm_at("AT+CSCS=\"GSM\"", NULL, 0, 2000);
    mdm_poll();
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", num);
    send(cmd);
    send("\r");
    if (!wait_prompt(5000)) {
        printf("[4G] SMS: khong co dau nhac '>'\n");
        return false;
    }
    send(text);
    ser_write(SER_SIM, "\x1A", 1);                   /* Ctrl+Z = gui */

    bool ok = wait_urc("+CMGS:", r, sizeof(r), 60000);
    printf("[4G] SMS toi %s: %s\n", num, ok ? "DA GUI" : "LOI");
    return ok;
}
