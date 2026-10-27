# Xem du lieu cac node LoRa ma gateway nhan duoc, giai ma JSON thanh dong de doc.
# Doc log gateway qua ST-Link moi -Interval giay; Ctrl+C de dung.
# Dung: powershell -ExecutionPolicy Bypass -File tools\nodewatch.ps1
#       ... nodewatch.ps1 -Node 349CE1DF        chi xem 1 node
#       ... nodewatch.ps1 -Raw                  in kem JSON goc
#       ... nodewatch.ps1 -NoHistory            bo qua cac goi da co trong log luc bat dau
param(
    [string]$Node = "",
    [int]$Interval = 5,
    [switch]$Raw,
    [switch]$NoHistory,
    [string]$Elf = "",
    [string]$Sn = ""
)

. "$PSScriptRoot\swd_common.ps1"
Initialize-Swd $Elf $Sn

$chgText = @{ 0 = "khong sac"; 1 = "trickle"; 2 = "pre-charge"; 3 = "sac CC"; 4 = "sac CV"; 6 = "top-off"; 7 = "day" }

# Log firmware khong co gio: goi da nam san trong log luc bat dau thi ghi "truoc"
$History = $false
function Stamp { if ($script:History) { return "truoc   " } return (Get-Date -Format "HH:mm:ss") }

function Fmt-Fixed($v, [int]$div, [int]$dec) {
    if ($null -eq $v) { return "-" }
    return ([double]$v / $div).ToString("F$dec", [Globalization.CultureInfo]::InvariantCulture)
}

function Show-Packet([string]$id, [int]$rssi, [int]$snr, [string]$json) {
    try { $j = $json | ConvertFrom-Json } catch { Write-Host "$id  (JSON loi) $json" -ForegroundColor Red; return }
    $t = Stamp
    $head = "{0} {1} n={2,-5} RSSI {3,4} SNR {4,3}" -f $t, $id, $j.n, $rssi, $snr
    if ($null -ne $j.rs) { $head += " (node nghe GW $($j.rs))" }
    Write-Host $head -ForegroundColor Cyan

    $env = @()
    if ($null -ne $j.t) { $env += "Nhiet do $(Fmt-Fixed $j.t 10 1) C" }
    if ($null -ne $j.h) { $env += "Do am $(Fmt-Fixed $j.h 10 1) %" }
    if ($null -ne $j.rain) { $env += "Mua: " + $(if ($j.rain -eq 1) { "CO" } else { "khong" }) }
    if ($null -ne $j.rne) { $env += "Cam bien mua LOI: $($j.rne)" }
    if ($env) { Write-Host ("    " + ($env -join " | ")) }

    if ($null -ne $j.vb) {
        $chg = $chgText[[int]$j.chg]; if (-not $chg) { $chg = "ma $($j.chg)" }
        $bat = "Pin $(Fmt-Fixed $j.vb 1000 2) V  $($j.ib) mA  $($j.bp) %  | Nguon vao $(Fmt-Fixed $j.vin 1000 2) V  | Sac: $chg"
        if ($null -ne $j.mp) { $bat += "  MPPT $(Fmt-Fixed $j.mp 1000 1) V" }
        Write-Host ("    " + $bat)
        if ($null -ne $j.bf) { Write-Host "    IC sac bao loi: $($j.bf)" -ForegroundColor Red }
    }

    $io = @()
    if ($j.in) { $io += "IN " + ($j.in -join "") }
    if ($j.rl) { $io += "RELAY " + ($j.rl -join "") }
    if ($j.f1) { $io += "F1 $($j.f1[0]) mL/p tong $($j.f1[1]) mL" }
    if ($j.f2) { $io += "F2 $($j.f2[0]) mL/p tong $($j.f2[1]) mL" }
    if ($null -ne $j.up) { $io += "chay $([int]($j.up / 60)) phut" }
    if ($io) { Write-Host ("    " + ($io -join " | ")) }

    if ($Raw) { Write-Host ("    " + $json) -ForegroundColor DarkGray }
}

function Handle-Line([string]$l) {
    if ($l -match '^\[NODE ([0-9A-Fa-f]+)\] RSSI (-?\d+) SNR (-?\d+): (\{.*\})$') {
        if ($Node -and $Matches[1] -ne $Node) { return }
        Show-Packet $Matches[1] ([int]$Matches[2]) ([int]$Matches[3]) $Matches[4]
        return
    }
    if ($l -match '^\[NODE ([0-9A-Fa-f]+)\] (.*)$') {
        if ($Node -and $Matches[1] -ne $Node) { return }
        $c = if ($Matches[2] -match 'OFFLINE|mat \d+ goi|khoi dong lai') { "Yellow" } else { "Green" }
        Write-Host ("{0} {1} {2}" -f (Stamp), $Matches[1], $Matches[2]) -ForegroundColor $c
        return
    }
    if ($l -match '^\[LORA\] (.*)$') {
        if ($Node -and $l -notmatch $Node) { return }
        Write-Host ("{0} LoRa: {1}" -f (Stamp), $Matches[1]) -ForegroundColor DarkGray
    }
}

Write-Host "Theo doi node LoRa qua log gateway (moi $Interval s, Ctrl+C de dung)..."
$last = -1
$partial = ""
$swdErr = $false
while ($true) {
    $log = Read-SwdLog
    if (-not $log) {
        if (-not $swdErr) { Write-Host "Khong doc duoc SWD (ST-Link rut / board mat nguon?)" -ForegroundColor Red; $swdErr = $true }
        Start-Sleep -Seconds $Interval
        continue
    }
    if ($swdErr) { Write-Host "Doc SWD lai duoc" -ForegroundColor Green; $swdErr = $false }

    $script:History = ($last -lt 0)
    if ($last -lt 0) {
        $from = if ($NoHistory) { $log.Pos } else { $log.Start }
    } elseif ($log.Pos -lt $last) {
        Write-Host "--- gateway vua khoi dong lai ---" -ForegroundColor Yellow
        $from = $log.Start; $partial = ""
    } elseif ($last -lt $log.Start) {
        Write-Host "(mat $($log.Start - $last) byte log vi doc khong kip, giam -Interval)" -ForegroundColor Yellow
        $from = $log.Start; $partial = ""
    } else {
        $from = $last
    }

    $text = $partial + $log.Text.Substring([int]($from - $log.Start))
    $lines = $text -split "\r?\n"
    $partial = $lines[-1]
    if ($lines.Count -gt 1) {
        foreach ($l in $lines[0..($lines.Count - 2)]) { if ($l) { Handle-Line $l } }
    }
    $last = $log.Pos
    Start-Sleep -Seconds $Interval
}
