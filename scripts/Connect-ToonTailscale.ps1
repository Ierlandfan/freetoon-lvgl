<#
.SYNOPSIS
    One-click Tailscale bring-up for an Eneco Toon — native PowerShell port.

.DESCRIPTION
    Run on a Windows laptop on the SAME LAN as the Toon. Auto-discovers the
    Toon on your network (or you pass/enter its IP), then uses the built-in
    OpenSSH client (ssh.exe) to install the static arm Tailscale build into
    /mnt/data on the Toon, wire an inittab respawn row, and join your tailnet.

    No sshpass / nmap / WSL needed. ssh.exe prompts for the Toon password
    interactively (nothing is stored).

.PARAMETER ToonIp
    The Toon's LAN address. Omit to auto-discover.

.PARAMETER User
    SSH user. Default 'root'.

.PARAMETER AuthKey
    Optional Tailscale auth-key (tskey-auth-...) for an unattended join.
    Generate at https://login.tailscale.com/admin/settings/keys (reusable +
    ephemeral recommended). If omitted, the join is interactive: a
    login.tailscale.com URL is printed — open it and approve the device.

.PARAMETER TsHostname
    Name shown in the Tailscale admin console. Default: the Toon's hostname.

.PARAMETER Subnet
    Override the /24 prefix to scan, e.g. '192.168.1'. Skips auto-detect.

.PARAMETER Uninstall
    Remove Tailscale (inittab row + binaries) from the Toon.

.EXAMPLE
    .\Connect-ToonTailscale.ps1
    Auto-discover, interactive login.

.EXAMPLE
    .\Connect-ToonTailscale.ps1 -ToonIp 192.168.1.50 -AuthKey tskey-auth-xxxx
    Pin the IP, unattended join.

.EXAMPLE
    .\Connect-ToonTailscale.ps1 -ToonIp 192.168.1.50 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$ToonIp,
    [string]$User = 'root',
    [string]$AuthKey = '',
    [string]$TsHostname = '',
    [string]$Subnet = '',
    [string]$TsVersion = '1.78.1',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$RawUrl = 'https://raw.githubusercontent.com/Ierlandfan/freetoon-lvgl/main/scripts/toon_tailscale.sh'

function Write-Log { param($m) Write-Host "[oneclick] $m" -ForegroundColor Cyan }
function Write-Err { param($m) Write-Host "[oneclick] ERROR: $m" -ForegroundColor Red }

function Test-IsIPv4 { param($s) return ($s -match '^(\d{1,3}\.){3}\d{1,3}$') }

# --- ssh.exe presence -------------------------------------------------------
if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    Write-Err "ssh.exe not found. Enable the OpenSSH client:"
    Write-Host "  Settings > Apps > Optional Features > Add > 'OpenSSH Client'" -ForegroundColor Yellow
    Write-Host "  (or:  Add-WindowsCapability -Online -Name OpenSSH.Client~~~~0.0.1.0 )" -ForegroundColor Yellow
    exit 2
}

# --- LAN subnet detection ---------------------------------------------------
function Get-LanSubnet {
    if ($Subnet) { return $Subnet }
    try {
        $cfg = Get-NetIPConfiguration -ErrorAction Stop |
            Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
            Select-Object -First 1
        if ($cfg -and $cfg.IPv4Address) {
            $ip = $cfg.IPv4Address.IPAddress
            if (Test-IsIPv4 $ip) { return ($ip -replace '\.\d+$','') }
        }
    } catch {}
    # Fallback: first private IPv4 on any up adapter
    try {
        $a = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object { $_.IPAddress -match '^(192\.168|10\.|172\.(1[6-9]|2\d|3[01]))' } |
            Select-Object -First 1
        if ($a) { return ($a.IPAddress -replace '\.\d+$','') }
    } catch {}
    return $null
}

# --- Parallel TCP :80 sweep via a runspace pool (works on PS 5.1 and 7) -----
function Find-HostsOnPort80 {
    param([string]$Prefix)
    $pool = [runspacefactory]::CreateRunspacePool(1, 64)
    $pool.Open()
    $probe = {
        param($ip)
        $c = New-Object System.Net.Sockets.TcpClient
        try {
            $ar = $c.BeginConnect($ip, 80, $null, $null)
            if ($ar.AsyncWaitHandle.WaitOne(400)) {
                $c.EndConnect($ar); $c.Close(); return $ip
            }
            $c.Close()
        } catch {} finally { $c.Dispose() }
        return $null
    }
    $jobs = foreach ($i in 1..254) {
        $ps = [powershell]::Create()
        $ps.RunspacePool = $pool
        [void]$ps.AddScript($probe).AddArgument("$Prefix.$i")
        [pscustomobject]@{ PS = $ps; Handle = $ps.BeginInvoke() }
    }
    $open = foreach ($j in $jobs) {
        $r = $j.PS.EndInvoke($j.Handle)
        $j.PS.Dispose()
        if ($r) { $r }
    }
    $pool.Close(); $pool.Dispose()
    return $open
}

# --- Toon HTTP fingerprint (no credentials needed) --------------------------
function Test-IsToon {
    param([string]$ip)
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/happ_thermstat?action=getThermostatInfo" `
             -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        if ($r.Content -match 'currentTemp') { return $true }
    } catch {}
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        if ($r.Content -match '(?i)qooxdoo|hcb|toon') { return $true }
    } catch {}
    return $false
}

function Find-Toons {
    $sub = Get-LanSubnet
    if (-not $sub) {
        Write-Err "could not detect your LAN subnet — use -Subnet 192.168.x or -ToonIp."
        return @()
    }
    Write-Log "scanning ${sub}.0/24 for a Toon (port 80 + HCB fingerprint) ..."
    $open = Find-HostsOnPort80 -Prefix $sub
    $found = foreach ($ip in $open) { if (Test-IsToon $ip) { $ip } }
    return @($found)
}

# --- Resolve the target Toon IP --------------------------------------------
if (-not $ToonIp) {
    $cands = Find-Toons
    if ($cands.Count -eq 1) {
        $ToonIp = $cands[0]
        Write-Log "found Toon at $ToonIp"
    }
    elseif ($cands.Count -gt 1) {
        Write-Log "found $($cands.Count) Toons:"
        for ($i = 0; $i -lt $cands.Count; $i++) { Write-Host ("   {0}) {1}" -f ($i+1), $cands[$i]) }
        $sel = Read-Host "Pick one [1-$($cands.Count)], or type an IP"
        if (Test-IsIPv4 $sel) { $ToonIp = $sel }
        elseif ($sel -match '^\d+$' -and [int]$sel -ge 1 -and [int]$sel -le $cands.Count) {
            $ToonIp = $cands[[int]$sel - 1]
        } else { Write-Err "invalid selection"; exit 1 }
    }
    else {
        Write-Log "no Toon auto-discovered on the LAN."
        do { $ToonIp = Read-Host "Enter the Toon's LAN IP" } until (Test-IsIPv4 $ToonIp)
    }
}
Write-Log "target Toon: $ToonIp"

# --- Build the remote command ----------------------------------------------
# The Toon downloads its own device-side script and runs it. Quote-safe:
# no multi-line here-doc through PowerShell.
$sshCommon = @(
    '-t',
    '-o', 'StrictHostKeyChecking=no',
    '-o', 'UserKnownHostsFile=NUL',
    '-o', 'LogLevel=ERROR',
    "$User@$ToonIp"
)

if ($Uninstall) {
    Write-Log "uninstalling Tailscale from $ToonIp ..."
    $remote = "curl -fsSL -o /tmp/ts.sh '$RawUrl' && sh /tmp/ts.sh --uninstall"
    & ssh @sshCommon $remote
    Write-Log "done."
    exit 0
}

Write-Log "connecting to $User@$ToonIp (you'll be asked for the SSH password) ..."
if (-not $AuthKey) {
    Write-Log "interactive join: a login.tailscale.com URL will appear below — open it in your browser."
}
$remote = "curl -fsSL -o /tmp/ts.sh '$RawUrl' && AUTHKEY='$AuthKey' HOSTNAME='$TsHostname' TS_VERSION='$TsVersion' sh /tmp/ts.sh"
& ssh @sshCommon $remote

Write-Host ""
Write-Log "Done. From any device on your tailnet:"
Write-Host "  ssh $User@<the-100.x-IP-printed-above>"
Write-Host "  http://<the-100.x-IP-printed-above>:10081/   (PWA)"
