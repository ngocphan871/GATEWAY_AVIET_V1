# Gui 1 lenh console cho gateway qua ST-Link roi in phan log moi sinh ra.
# Dung: powershell -ExecutionPolicy Bypass -File tools\sendcmd.ps1 "nodes"
#       ... sendcmd.ps1 "node 349CE1DF relay 1 1"     (gui lenh xuong node qua LoRa)
#       ... sendcmd.ps1 "status" | "help" | "time" ...
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Cmd,
    [int]$WaitMs = 3000,
    [string]$Elf = "",
    [string]$Sn = ""
)

. "$PSScriptRoot\swd_common.ps1"
Initialize-Swd $Elf $Sn

$before = Read-SwdLog
if (-not $before) { Write-Output "Khong doc duoc qua SWD (ST-Link chua cam / board mat nguon?)"; exit 1 }
Send-SwdCmd $Cmd
if (-not (Wait-SwdCmdFree)) { Write-Output "Gateway chua nhan lenh sau 60 s (dang ban 4G?)" }
Start-Sleep -Milliseconds $WaitMs
$after = Read-SwdLog
if (-not $after) { Write-Output "Khong doc lai duoc log"; exit 1 }

# Chi in phan log tu sau luc gui lenh
$skip = $before.Pos - $after.Start
if ($skip -gt 0 -and $skip -le $after.Text.Length) { Write-Output $after.Text.Substring([int]$skip) }
else { Write-Output $after.Text }
