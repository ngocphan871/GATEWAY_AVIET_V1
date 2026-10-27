# In log printf cua gateway (toi da 4 KB cuoi) qua ST-Link.
# Dung: powershell -ExecutionPolicy Bypass -File tools\readlog.ps1 [-Sn <serial ST-Link>] [-Elf <file.elf>]
param([string]$Elf = "", [string]$Sn = "")

. "$PSScriptRoot\swd_common.ps1"
Initialize-Swd $Elf $Sn
$log = Read-SwdLog
if (-not $log) { Write-Output "Khong doc duoc qua SWD (ST-Link chua cam / board mat nguon?)"; exit 1 }
Write-Output "=== dbg_log_pos = $($log.Pos) ==="
Write-Output $log.Text
