# GATEWAY_AVIET_V1

Firmware gateway STM32F407VET6 + 4G EC800M + LoRa S68F (SX1268), lam viec voi node `NODE_AVIET_V1`.
Cau hinh o `App/Inc/config.h`.

## Xem du lieu qua ST-Link (khong can USB-TTL)

Cam ST-Link vao gateway, build xong (`build/Debug/GATEWAY_VIET_V1.elf` phai dung ban dang nap), roi chay trong thu muc project:

```
powershell -ExecutionPolicy Bypass -File tools\nodewatch.ps1
```
Xem lien tuc du lieu cac node LoRa (nhiet do, do am, pin, sac, mua, dau vao/relay, RSSI). Ctrl+C de dung.
Tuy chon: `-Node 349CE1DF` chi xem 1 node, `-Raw` in kem JSON goc, `-NoHistory` bo qua goi cu, `-Interval 5`.

```
powershell -ExecutionPolicy Bypass -File tools\sendcmd.ps1 "nodes"
```
Gui 1 lenh console va in ket qua. Vi du: `"nodes"` (bang node), `"node 349CE1DF send"` (xin node bao cao ngay),
`"node 349CE1DF relay 1 1"`, `"status"`, `"help"`.

```
powershell -ExecutionPolicy Bypass -File tools\readlog.ps1
```
In 4 KB log cuoi cua gateway.

Cam nhieu ST-Link cung luc thi them `-Sn <serial>` (xem serial bang `STM32_Programmer_CLI -l st`).

## MQTT

Topic `aviet/gw/<IMEI>/...`: `data` (gateway), `node/<id>` (goi node + rssi/snr, `{"id":..,"online":0}` khi mat node),
`lora` (goi la, hex), `cmd` (lenh: `{"relay1":1}`, `{"node":"349CE1DF","cmd":"relay 1 1"}`).
