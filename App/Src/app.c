/*
 * Ung dung gateway:
 *   - Doc 3 dau vao opto, 2 cam bien luu luong, thanh ghi Modbus qua RS485
 *   - Nhan goi LoRa tu cac node (lora_node.c), gui lenh xuong node
 *   - Gui tat ca len MQTT qua 4G (EC800M), nhan lenh dieu khien relay
 *     Topic: <base>/<IMEI>/data      du lieu gateway
 *            <base>/<IMEI>/node/<id> goi bao cao cua node (+ rssi/snr), {"id":..,"online":0/1}
 *            <base>/<IMEI>/lora      goi LoRa la (hex)
 *            <base>/<IMEI>/cmd       lenh: {"relay1":1} | {"node":"349CE1DF","cmd":"relay 1 1"} | chu
 *   - Hien thi trang thai len TFT
 *   - Console tren USART1 (CN6) de thu tung khoi, go "help"
 */
#include "app.h"
#include "board.h"
#include "clock.h"
#include "config.h"
#include "ec800m.h"
#include "lora_node.h"
#include "modbus.h"
#include "serial.h"
#include "sx1262.h"
#include "tft.h"
#include "w25q.h"
#include "main.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern I2C_HandleTypeDef hi2c2;

static bool lora_ok, flash_ok, tft_ok;
static int csq = 99;
static char topic_data[64], topic_cmd[64];

static mb_status_t mb_st = MB_ERR_TIMEOUT;
static uint16_t mb_regs[MB_MAX_REGS];

static uint32_t t_1s, t_mb, t_pub, t_net, t_csq;
static uint32_t t_time_try, t_time_ok;
static bool time_synced;            /* da lay duoc gio mang tu luc bat may */
static bool net_tried;
static bool pub_now;

static char con_buf[128];
static uint8_t con_len;

/* ---------------------------------------------------------------- */

static void i2c_scan(void)
{
    int n = 0;
    printf("I2C2 (PB10/PB11):");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(a << 1), 1, 2) == HAL_OK) {
            printf(" 0x%02X", a);
            n++;
        }
    }
    printf(n ? "\n" : " khong co thiet bi\n");
}

static void flash_test(void)
{
    if (!flash_ok) {
        printf("Flash: chua nhan chip (kiem tra day FLASH-CS -> PA4)\n");
        return;
    }
    uint32_t addr = w25q_size() - W25Q_SECTOR_SIZE;   /* sector cuoi */
    const char msg[] = "AVIET-GW flash test";
    char back[sizeof(msg)] = { 0 };
    w25q_erase_sector(addr);
    w25q_write(addr, msg, sizeof(msg));
    w25q_read(addr, back, sizeof(back));
    printf("Flash ghi/doc sector 0x%06lX: %s\n", (unsigned long)addr,
           memcmp(msg, back, sizeof(msg)) == 0 ? "OK" : "SAI");
}

static void poll_modbus(void)
{
    mb_st = mb_read_regs(CFG_MB_SLAVE, CFG_MB_FUNC, CFG_MB_START_REG, CFG_MB_REG_COUNT, mb_regs);
}

static void print_status(void)
{
    printf("--- Trang thai ---\n");
    printf("IN1..3 : %d %d %d\n", board_input_get(1), board_input_get(2), board_input_get(3));
    printf("RELAY  : %d %d\n", board_relay_get(1), board_relay_get(2));
    for (int i = 1; i <= 2; i++) {
        printf("FLOW%d  : %lu mL/phut, tong %lu mL (%lu xung)\n", i,
               (unsigned long)board_flow_ml_per_min(i), (unsigned long)board_flow_total_ml(i),
               (unsigned long)board_flow_pulses(i));
    }
    printf("Modbus : slave %d FC%d -> %s", CFG_MB_SLAVE, CFG_MB_FUNC, mb_status_str(mb_st));
    if (mb_st == MB_OK) {
        for (int i = 0; i < CFG_MB_REG_COUNT; i++) { printf(" %u", mb_regs[i]); }
    }
    printf("\n");
    printf("LoRa   : %s, da nhan %lu goi, RSSI cuoi %d dBm, node online %d/%d (lenh \"nodes\")\n",
           lora_ok ? "OK" : "LOI", (unsigned long)lora_rx_total(), lora_last_rssi(),
           lora_node_online(), lora_node_known());
    printf("4G     : %s, IMEI %s, CSQ %d, MQTT %s\n", mdm_is_on() ? "bat" : "tat",
           mdm_imei(), csq, mdm_mqtt_connected() ? "OK" : "chua ket noi");
    printf("Flash  : %s, TFT: %s\n", flash_ok ? "OK" : "LOI", tft_ok ? "da khoi tao" : "-");
    char ts[24];
    clk_format(ts, sizeof(ts));
    printf("Gio    : %s\n", ts[0] ? ts : "chua dat (cho gio tu mang 4G hoac lenh time)");
}

/* Lay gio tu mang 4G cho RTC: lan dau ngay khi co, sau do moi CFG_TIME_SYNC_MS */
static void time_sync_task(void)
{
    uint32_t now = HAL_GetTick();
    if (!mdm_sim_ready()) { return; }
    if (time_synced && (now - t_time_ok) < CFG_TIME_SYNC_MS) { return; }
    if ((now - t_time_try) < 30000u) { return; }      /* mang chua co gio: 30 s thu lai */
    t_time_try = now;

    clk_time_t t;
    if (!mdm_get_time(&t)) { return; }
    if (clk_set(&t)) {
        time_synced = true;
        t_time_ok = now;
        printf("[RTC] Dong bo gio mang: %04u-%02u-%02u %02u:%02u:%02u\n",
               t.year, t.mon, t.day, t.hour, t.min, t.sec);
    }
}

static int build_json(char *buf, size_t len)
{
    char ts[24];
    clk_format(ts, sizeof(ts));
    int n = snprintf(buf, len, "{");
    if (ts[0]) { n += snprintf(buf + n, len - (size_t)n, "\"ts\":\"%s\",", ts); }
    n += snprintf(buf + n, len - (size_t)n,
        "\"up\":%lu,\"in\":[%d,%d,%d],\"rl\":[%d,%d],"
        "\"f1\":{\"mlm\":%lu,\"ml\":%lu},\"f2\":{\"mlm\":%lu,\"ml\":%lu},\"csq\":%d,\"nodes\":%d,\"mb\":",
        (unsigned long)(HAL_GetTick() / 1000u),
        board_input_get(1), board_input_get(2), board_input_get(3),
        board_relay_get(1), board_relay_get(2),
        (unsigned long)board_flow_ml_per_min(1), (unsigned long)board_flow_total_ml(1),
        (unsigned long)board_flow_ml_per_min(2), (unsigned long)board_flow_total_ml(2),
        csq, lora_node_online());
    if (mb_st == MB_OK) {
        n += snprintf(buf + n, len - (size_t)n, "[");
        for (int i = 0; i < CFG_MB_REG_COUNT; i++) {
            n += snprintf(buf + n, len - (size_t)n, i ? ",%u" : "%u", mb_regs[i]);
        }
        n += snprintf(buf + n, len - (size_t)n, "]}");
    } else {
        n += snprintf(buf + n, len - (size_t)n, "null,\"mb_err\":\"%s\"}", mb_status_str(mb_st));
    }
    return n;
}

bool app_publish(const char *suffix, const char *payload)
{
    char topic[96];
    if (!mdm_mqtt_connected()) { return false; }
    snprintf(topic, sizeof(topic), "%s/%s/%s", CFG_TOPIC_BASE, mdm_imei(), suffix);
    if (!mdm_mqtt_publish(topic, payload)) {
        printf("[PUB] %s that bai\n", topic);
        return false;
    }
    return true;
}

static void publish_data(void)
{
    char json[512];
    build_json(json, sizeof(json));
    printf("[PUB] %s\n", json);
    if (mdm_mqtt_connected() && !mdm_mqtt_publish(topic_data, json)) {
        printf("[PUB] that bai\n");
    }
}

/* ---------------- lenh: tu console hoac tu MQTT ---------------- */

static void cmd_exec(char *s, bool remote)
{
    char *argv[6];
    int argc = 0;

    /* Lenh cho node: {"node":"349CE1DF","cmd":"relay 1 1"} -> LoRa "@349CE1DF relay 1 1" */
    if (s[0] == '{' && strstr(s, "\"node\"")) {
        char id[12], ncmd[100];
        if (!json_get_str(s, "node", id, sizeof(id)) || !json_get_str(s, "cmd", ncmd, sizeof(ncmd)) ||
            id[0] == '\0' || ncmd[0] == '\0') {
            printf("Lenh node sai dinh dang, can {\"node\":\"<id>\",\"cmd\":\"<lenh>\"}\n");
            return;
        }
        lora_node_send(id, ncmd);
        return;
    }

    /* JSON tu server: {"relay1":1,"relay2":0} */
    if (s[0] == '{') {
        for (int i = 1; i <= 2; i++) {
            char key[12];
            snprintf(key, sizeof(key), "\"relay%d\"", i);
            char *p = strstr(s, key);
            if (p && (p = strchr(p, ':')) != NULL) {
                board_relay_set(i, atoi(p + 1) != 0);
                printf("Relay%d = %d\n", i, board_relay_get(i));
            }
        }
        pub_now = true;
        return;
    }

    for (char *tok = strtok(s, " "); tok && argc < 6; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }
    if (argc == 0) { return; }

    if (strcmp(argv[0], "relay") == 0 && argc == 3) {
        board_relay_set(atoi(argv[1]), atoi(argv[2]) != 0);
        printf("Relay%s = %d\n", argv[1], board_relay_get(atoi(argv[1])));
        pub_now = true;
    } else if (strcmp(argv[0], "status") == 0) {
        print_status();
    } else if (strcmp(argv[0], "pub") == 0) {
        pub_now = true;
    } else if (strcmp(argv[0], "lora") == 0 && argc >= 2) {
        /* gom lai phan con lai cua dong lam noi dung */
        char msg[128] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) { strncat(msg, " ", sizeof(msg) - strlen(msg) - 1); }
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        lora_send_raw(msg);
    } else if (strcmp(argv[0], "node") == 0 && argc >= 3) {
        /* node <id|*> <lenh...>  ==  lora @<id> <lenh...> */
        char ncmd[100] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) { strncat(ncmd, " ", sizeof(ncmd) - strlen(ncmd) - 1); }
            strncat(ncmd, argv[i], sizeof(ncmd) - strlen(ncmd) - 1);
        }
        lora_node_send(argv[1], ncmd);
    } else if (strcmp(argv[0], "nodes") == 0) {
        lora_node_print();
    } else if (strcmp(argv[0], "mb") == 0 && argc == 5) {
        uint16_t v[MB_MAX_REGS];
        uint16_t cnt = (uint16_t)atoi(argv[4]);
        mb_status_t st = mb_read_regs((uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2]),
                                      (uint16_t)atoi(argv[3]), cnt, v);
        printf("Modbus: %s", mb_status_str(st));
        if (st == MB_OK) { for (int i = 0; i < cnt; i++) { printf(" %u", v[i]); } }
        if (st == MB_ERR_EXCEPTION) { printf(" ma %u", mb_last_exception()); }
        printf("\n");
    } else if (strcmp(argv[0], "mbw") == 0 && argc == 4) {
        mb_status_t st = mb_write_reg((uint8_t)atoi(argv[1]), (uint16_t)atoi(argv[2]),
                                      (uint16_t)atoi(argv[3]));
        printf("Modbus ghi: %s\n", mb_status_str(st));
    } else if (remote) {
        printf("Lenh tu xa khong hop le: %s\n", argv[0]);
    } else if (strcmp(argv[0], "at") == 0 && argc >= 2) {
        char cmd[100], r[400];
        snprintf(cmd, sizeof(cmd), "%s", argv[1]);
        for (int i = 2; i < argc; i++) {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        mdm_at_t st = mdm_at(cmd, r, sizeof(r), 10000);
        printf("%s%s\n", r, st == MDM_AT_OK ? "OK" : st == MDM_AT_ERROR ? "" : "[timeout]");
    } else if (strcmp(argv[0], "diag") == 0) {
        mdm_diag();
    } else if (strcmp(argv[0], "time") == 0) {
        char ts[24];
        if (argc == 3) {
            char both[24];
            clk_time_t t;
            snprintf(both, sizeof(both), "%s %s", argv[1], argv[2]);
            if (!clk_parse(both, &t) || !clk_set(&t)) {
                printf("Sai dinh dang: time YYYY-MM-DD HH:MM:SS\n");
                return;
            }
        }
        clk_format(ts, sizeof(ts));
        printf("Gio RTC: %s\n", ts[0] ? ts : "chua dat");
    } else if (strcmp(argv[0], "call") == 0 && argc >= 2) {
        uint32_t sec = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 30u;
        mdm_call(argv[1], sec * 1000u);
    } else if (strcmp(argv[0], "sms") == 0 && argc >= 3) {
        char msg[160] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) { strncat(msg, " ", sizeof(msg) - strlen(msg) - 1); }
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        mdm_sms_send(argv[1], msg);
    } else if (strcmp(argv[0], "i2c") == 0) {
        i2c_scan();
    } else if (strcmp(argv[0], "flash") == 0) {
        flash_test();
    } else if (strcmp(argv[0], "net") == 0) {
        t_net = HAL_GetTick() - CFG_RECONNECT_PERIOD_MS;    /* thu ket noi ngay */
        net_tried = false;
        mdm_sim_retry_now();                                /* va nhan lai SIM */
    } else {
        printf("Lenh:\n"
               "  status                  in trang thai\n"
               "  relay <1|2> <0|1>       dong/ngat relay\n"
               "  mb <slave> <fc> <addr> <n>   doc thanh ghi Modbus (fc 3/4)\n"
               "  mbw <slave> <addr> <val>     ghi 1 thanh ghi\n"
               "  lora <noi dung>         gui nguyen chuoi qua LoRa\n"
               "  node <id|*> <lenh>      gui lenh cho node (vd: node 349CE1DF relay 1 1)\n"
               "  nodes                   bang node LoRa (online, mat goi, RSSI)\n"
               "  at <lenh>               gui lenh AT xuong EC800M\n"
               "  net                     ket noi lai 4G/MQTT\n"
               "  diag                    chan doan SIM / song 4G\n"
               "  time [YYYY-MM-DD HH:MM:SS]  xem / dat gio RTC\n"
               "  call <so> [giay]        goi thoai, tu cup sau 30 s\n"
               "  sms <so> <noi dung>     nhan tin (chu khong dau)\n"
               "  pub                     gui du lieu len MQTT ngay\n"
               "  i2c                     quet bus I2C\n"
               "  flash                   thu ghi/doc W25Q32\n");
    }
}

void mdm_on_mqtt_message(const char *topic, const char *payload)
{
    char buf[128];
    printf("[MQTT] %s: %s\n", topic, payload);
    if (strcmp(topic, topic_cmd) != 0) { return; }
    snprintf(buf, sizeof(buf), "%s", payload);
    cmd_exec(buf, true);
}

/*
 * Hop lenh qua SWD: debugger ghi chuoi lenh vao swd_cmd (byte 0 ghi SAU CUNG),
 * firmware chay nhu go o console roi xoa byte 0. Dung khi khong co cap USB-TTL.
 */
volatile char swd_cmd[128];

static void swd_cmd_poll(void)
{
    char buf[sizeof(swd_cmd)];
    if (swd_cmd[0] == '\0') { return; }
    for (unsigned i = 0; i < sizeof(buf); i++) { buf[i] = swd_cmd[i]; }
    buf[sizeof(buf) - 1] = '\0';
    swd_cmd[0] = '\0';
    printf("[SWD] %s\n", buf);
    cmd_exec(buf, false);
}

static void console_poll(void)
{
    int c;
    while ((c = ser_getc(SER_DBG)) >= 0) {
        if (c == '\r' || c == '\n') {
            if (con_len) {
                con_buf[con_len] = '\0';
                printf("\n");
                cmd_exec(con_buf, false);
                con_len = 0;
            }
        } else if (c == 0x08 || c == 0x7F) {
            if (con_len) { con_len--; }
        } else if (isprint(c) && con_len < sizeof(con_buf) - 1) {
            con_buf[con_len++] = (char)c;
            putchar(c);         /* echo */
        }
    }
}

/* ---------------- man hinh ---------------- */

static void tft_line(int row, const char *s, uint16_t color)
{
    char buf[48];
    uint8_t sc = (tft_width() >= 240) ? 2 : 1;
    int cols = tft_width() / (6 * sc);
    if (cols > (int)sizeof(buf) - 1) { cols = sizeof(buf) - 1; }
    snprintf(buf, sizeof(buf), "%-*.*s", cols, cols, s);     /* dem trang de xoa chu cu */
    tft_text(0, (uint16_t)(4 + row * 10 * sc), buf, color, TFT_BLACK, sc);
}

static void tft_update(void)
{
    char s[48];
    uint32_t up = HAL_GetTick() / 1000u;

    tft_line(0, "AVIET GATEWAY V1", TFT_CYAN);
    snprintf(s, sizeof(s), "4G:%s CSQ:%d", mdm_mqtt_connected() ? "MQTT OK" : mdm_is_on() ? "ON" : "OFF", csq);
    tft_line(1, s, mdm_mqtt_connected() ? TFT_GREEN : TFT_YELLOW);
    snprintf(s, sizeof(s), "IN:%d%d%d  RELAY:%d%d", board_input_get(1), board_input_get(2),
             board_input_get(3), board_relay_get(1), board_relay_get(2));
    tft_line(2, s, TFT_WHITE);
    for (int i = 1; i <= 2; i++) {
        uint32_t r = board_flow_ml_per_min(i), t = board_flow_total_ml(i);
        snprintf(s, sizeof(s), "F%d:%lu.%02luL/m %lu.%luL", i,
                 (unsigned long)(r / 1000u), (unsigned long)((r % 1000u) / 10u),
                 (unsigned long)(t / 1000u), (unsigned long)((t % 1000u) / 100u));
        tft_line(2 + i, s, TFT_WHITE);
    }
    if (mb_st == MB_OK) {
        snprintf(s, sizeof(s), "MB:%u %u %u %u", mb_regs[0], mb_regs[1], mb_regs[2], mb_regs[3]);
    } else {
        snprintf(s, sizeof(s), "MB:%s", mb_status_str(mb_st));
    }
    tft_line(5, s, mb_st == MB_OK ? TFT_WHITE : TFT_RED);
    snprintf(s, sizeof(s), "LORA:%s NODE %d/%d %ddBm", lora_ok ? "OK" : "ERR",
             lora_node_online(), lora_node_known(), lora_last_rssi());
    tft_line(6, s, !lora_ok ? TFT_RED : (lora_node_online() < lora_node_known()) ? TFT_YELLOW : TFT_WHITE);
    if (clk_valid()) {
        clk_time_t t;
        clk_get(&t);
        snprintf(s, sizeof(s), "%02u/%02u/%04u %02u:%02u:%02u",
                 t.day, t.mon, t.year, t.hour, t.min, t.sec);
        tft_line(7, s, TFT_CYAN);
    } else {
        snprintf(s, sizeof(s), "UP %02lu:%02lu:%02lu", (unsigned long)(up / 3600u),
                 (unsigned long)((up / 60u) % 60u), (unsigned long)(up % 60u));
        tft_line(7, s, TFT_GRAY);
    }
}

/* ---------------- mang ---------------- */

static void net_task(void)
{
    if (mdm_mqtt_connected()) { return; }
    if (net_tried && (HAL_GetTick() - t_net) < CFG_RECONNECT_PERIOD_MS) { return; }
    net_tried = true;
    t_net = HAL_GetTick();

    /* An toan khi module dang chay: chi bam PWRKEY neu khong tra loi AT */
    if (!mdm_power_on()) {
        mdm_hard_reset();
        return;
    }
    csq = mdm_csq();
    printf("[4G] CSQ = %d\n", csq);

    char client[40];
    snprintf(client, sizeof(client), "gw-%s", mdm_imei());
    snprintf(topic_data, sizeof(topic_data), "%s/%s/data", CFG_TOPIC_BASE, mdm_imei());
    snprintf(topic_cmd,  sizeof(topic_cmd),  "%s/%s/cmd",  CFG_TOPIC_BASE, mdm_imei());
    if (mdm_mqtt_connect(client, topic_cmd)) {
        printf("[4G] Topic: %s | .../node/<id> | .../lora | %s\n", topic_data, topic_cmd);
        pub_now = true;
    }
}

/* IN1 -> RELAY1, IN2 -> RELAY2, co chong doi */
static void input_relay_task(void)
{
#if CFG_INPUT_RELAY_LINK
    static int8_t applied[2] = { -1, -1 };     /* -1: chua ap dung lan nao */
    static bool last_raw[2];
    static uint32_t t_change[2];
    uint32_t now = HAL_GetTick();

    for (int i = 0; i < 2; i++) {
        bool raw = board_input_get(i + 1);
        if (raw != last_raw[i]) {
            last_raw[i] = raw;
            t_change[i] = now;
            continue;
        }
        if ((now - t_change[i]) < CFG_INPUT_DEBOUNCE_MS || applied[i] == (int8_t)raw) {
            continue;
        }
        applied[i] = (int8_t)raw;
        board_relay_set(i + 1, raw);
        printf("IN%d = %d -> RELAY%d = %d\n", i + 1, raw, i + 1, raw);
        pub_now = true;
    }
#endif
}

/* Driver 4G co the cho hang chuc giay: van cho relay theo dau vao va hut goi LoRa vao
 * hang doi (SX1268 chi giu 1 goi). KHONG publish o day - driver modem dang ban. */
void mdm_idle(void)
{
    input_relay_task();
    lora_rx_poll();
}

/* ---------------------------------------------------------------- */

void app_init(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ser_init();
    board_led_set(true);

    printf("\n\n==== AVIET GATEWAY V1 - STM32F407VET6 @ %lu MHz ====\n",
           (unsigned long)(HAL_RCC_GetHCLKFreq() / 1000000u));

    flash_ok = w25q_init();
    printf("Flash W25Q: JEDEC 0x%06lX -> %s\n", (unsigned long)w25q_jedec_id(),
           flash_ok ? "OK" : "KHONG THAY (FLASH-CS chua noi PA4?)");

    lora_ok = sx1262_init();
    if (lora_ok) {
        uint16_t e = sx1262_device_errors();
        printf("LoRa S68F/SX1268: OK %lu Hz SF%d BW%d, loi thiet bi 0x%04X%s\n",
               (unsigned long)CFG_LORA_FREQ_HZ, CFG_LORA_SF, CFG_LORA_BW, e,
               (e & 0x20u) ? " (XOSC loi -> kiem tra TCXO)" : "");
        lora_node_init();
        sx1262_start_rx();
    } else {
        printf("LoRa SX1262: KHONG tra loi SPI\n");
    }

    tft_init();
    tft_ok = true;
    tft_update();

    i2c_scan();
    board_flow_update();
    printf("Go \"help\" de xem lenh.\n");

    t_1s = t_mb = t_pub = HAL_GetTick();
}

void app_loop(void)
{
    uint32_t now = HAL_GetTick();

    console_poll();
    swd_cmd_poll();
    input_relay_task();
    mdm_poll();
    lora_node_task();

    if (now - t_1s >= 1000u) {
        t_1s = now;
        board_flow_update();
        board_led_toggle();
        tft_update();
    }
    /* CSQ doc ngay sau CFUN 0/1 luon la 99: cap nhat lai dinh ky */
    if (mdm_is_on() && now - t_csq >= 10000u) {
        t_csq = now;
        int v = mdm_csq();
        if (v != csq) { printf("[4G] CSQ %d -> %d\n", csq, v); }
        csq = v;
    }
    if (now - t_mb >= CFG_MB_POLL_PERIOD_MS) {
        t_mb = now;
        poll_modbus();
    }

    net_task();
    time_sync_task();

    if (pub_now || (now - t_pub) >= CFG_PUBLISH_PERIOD_MS) {
        pub_now = false;
        t_pub = now;
        if (mdm_mqtt_connected()) { csq = mdm_csq(); }
        publish_data();
    }
}
