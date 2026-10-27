# Ham dung chung cho cac script doc/ghi RAM gateway qua ST-Link (SWD), khong can USB-TTL.
# Cac script khac nap file nay bang:  . "$PSScriptRoot\swd_common.ps1"
#
# Firmware chep moi ky tu printf vao dbg_log[4096] (vong), dbg_log_pos = tong so byte da ghi.
# Lenh console ghi vao swd_cmd[128]: firmware chay nhu go o CN6 roi xoa byte 0.
# Dia chi bien lay tu file .elf nen phai dung dung file .elf dang nap tren board.

function Find-FirstPath([string[]]$cands) {
    foreach ($c in $cands) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

$SwdCli = Find-FirstPath @(
    "C:\ST\STM32CubeCLT_1.22.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe",
    "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe")
$SwdNm = Find-FirstPath @("C:\ST\STM32CubeCLT_1.22.0\GNU-tools-for-STM32\bin\arm-none-eabi-nm.exe")
$SwdLogSize = 4096

# Elf: mac dinh build\Debug\GATEWAY_VIET_V1.elf; Sn: so serial ST-Link khi cam nhieu cai
function Initialize-Swd([string]$Elf, [string]$Sn) {
    if (-not $SwdCli) { throw "Khong tim thay STM32_Programmer_CLI.exe (cai STM32CubeCLT/CubeProgrammer)" }
    if (-not $SwdNm) { throw "Khong tim thay arm-none-eabi-nm.exe (C:\ST\STM32CubeCLT_1.22.0)" }
    if (-not $Elf) { $Elf = Join-Path $PSScriptRoot "..\build\Debug\GATEWAY_VIET_V1.elf" }
    if (-not (Test-Path $Elf)) { throw "Khong thay $Elf - build truoc (cmake --build --preset Debug)" }

    $script:SwdConn = @("-c", "port=SWD", "mode=HOTPLUG", "freq=950")
    if ($Sn) { $script:SwdConn += "sn=$Sn" }

    $syms = & $SwdNm $Elf
    $script:SwdSym = @{}
    foreach ($name in @("dbg_log", "dbg_log_pos", "swd_cmd")) {
        $line = ($syms | Select-String " $name$").Line
        if (-not $line) { throw "Khong thay bien $name trong $Elf (firmware cu?)" }
        $script:SwdSym[$name] = [Convert]::ToUInt32(($line -split ' ')[0], 16)
    }
}

function Read-SwdBytes([uint32]$Addr, [int]$Len) {
    $out = & $SwdCli @SwdConn -r8 ("0x{0:X8}" -f $Addr) $Len 2>&1
    $bytes = New-Object System.Collections.Generic.List[byte]
    foreach ($l in $out) {
        if ("$l" -match '^0x[0-9A-Fa-f]{8}\s*:\s*(.*)$') {
            foreach ($h in ($Matches[1].Trim() -split '\s+')) {
                if ($h -match '^[0-9A-Fa-f]{2}$') { $bytes.Add([Convert]::ToByte($h, 16)) }
            }
        }
    }
    return , $bytes.ToArray()
}

# Tra ve @{ Pos = tong byte da ghi; Start = vi tri tuyet doi cua ky tu dau Text; Text = log }
# hoac $null neu khong doc duoc SWD
function Read-SwdLog {
    $p = Read-SwdBytes $SwdSym["dbg_log_pos"] 4
    if ($p.Length -lt 4) { return $null }
    $pos = [BitConverter]::ToUInt32($p, 0)
    $buf = Read-SwdBytes $SwdSym["dbg_log"] $SwdLogSize
    if ($buf.Length -lt $SwdLogSize) { return $null }

    if ($pos -le $SwdLogSize) {
        $data = if ($pos -gt 0) { $buf[0..([int]$pos - 1)] } else { @() }
        $start = 0
    } else {
        $s = [int]($pos % $SwdLogSize)
        $data = if ($s -gt 0) { $buf[$s..($SwdLogSize - 1)] + $buf[0..($s - 1)] } else { $buf }
        $start = [int64]$pos - $SwdLogSize
    }
    return @{ Pos = [int64]$pos; Start = [int64]$start; Text = [Text.Encoding]::ASCII.GetString([byte[]]$data) }
}

# Cho firmware lay lenh truoc ra khoi swd_cmd (byte 0 ve 0). Firmware chi doc hop lenh o
# vong lap chinh, ma vong lap co the bi 4G giu hang chuc giay -> ghi de se mat lenh truoc.
function Wait-SwdCmdFree([int]$TimeoutSec = 60) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        $b = Read-SwdBytes $SwdSym["swd_cmd"] 1
        if ($b.Length -eq 1 -and $b[0] -eq 0) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# Ghi 1 lenh console vao swd_cmd. Word 0 (chua byte dau) ghi SAU CUNG.
function Send-SwdCmd([string]$Cmd) {
    $b = [Text.Encoding]::ASCII.GetBytes($Cmd)
    if ($b.Length -gt 126) { throw "Lenh qua dai (toi da 126 ky tu)" }
    if (-not (Wait-SwdCmdFree)) { throw "Gateway chua lay lenh truoc sau 60 s (dang ban 4G / treo?)" }
    $padded = New-Object byte[] ([Math]::Ceiling(($b.Length + 1) / 4) * 4)
    [Array]::Copy($b, $padded, $b.Length)
    $addr = $SwdSym["swd_cmd"]
    for ($i = $padded.Length / 4 - 1; $i -ge 0; $i--) {
        $v = [BitConverter]::ToUInt32($padded, $i * 4)
        $r = & $SwdCli @SwdConn -w32 ("0x{0:X8}" -f ($addr + 4 * $i)) ("0x{0:X8}" -f $v) 2>&1
        # Word 0: firmware nhan lenh va xoa byte 0 ngay nen buoc doc lai cua CLI co the
        # bao "Failed to download" du da ghi xong -> bo qua loi do o word 0.
        $txt = $r -join "`n"
        if ($txt -match 'Error' -and ($i -gt 0 -or $txt -notmatch 'Failed to download')) {
            throw "Ghi swd_cmd loi: $(($r | Select-String Error) -join ' ')"
        }
    }
}
