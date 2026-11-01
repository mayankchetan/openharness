# End-to-end checks for the dial phone bridge (plan task B11). Windows PowerShell 5.1 or PowerShell 7.
#
# What it does, and what it does not:
#   - It READS log files the daemon already writes: dial-YYYYMMDD.log under ~/.harness/logs/usb-<MAC>/ (the dial's own lines and
#     the session's "[daemon]" lines, Z stamps) and the daemon log ~/.harness/cli/data/harness.log (the "[cable]" lines, local-time
#     stamps; the default for -DaemonLog when the file exists). It opens no serial port: no ad-hoc
#     serial scripts (workbench rule); dial lines reach the log only through the daemon.
#   - It runs `adb reverse` for the walking skeleton (the phone reaches the desktop's loopback endpoint through the USB cable)
#     and checks that the relay endpoint listens on loopback only. It starts no daemon and calls no network service.
#   - It judges; it does not drive. A person speaks to the dial. Every verdict prints the path of an evidence file with the raw
#     matched lines, so a number in a report is read from that file, not typed.
#
# Commands (-Command):
#   m1            check the walking-skeleton pass of plan 3.1 from the logs:
#                   coc-up          the dial's ble_link logged "channel up"
#                   tls-psk-up      the dial's ble_link logged "link up #N handshake_ms=" (a finished TLS-PSK handshake; the
#                                   firmware offers only ECDHE-PSK-AES128-CBC-SHA256, 0xC037, cable_xport_tls.c s_suites, so
#                                   a finished handshake is that suite: the line does not name it)
#                   tunnel-session  the daemon says "tunnel dial <MAC> authenticated from tunnel", and the dial's session
#                                   then said hello ("[daemon] dial <MAC> ... on fw"). Reads -DaemonLog (default: harness.log); SKIP when there is none.
#                   vox-turn        every "vox: VOX n=" line in the window has codec=adpcm, overruns=0, dropped=0 and a
#                                   transcript time, and no VOX DONE line shows cum_rx_overruns above 0; at least one line
#                   int-min         int_min on those lines is reported and at least 8192 B (gate GB3 floor)
#                   order           the four moments above came in that order
#   listen-check  the relay endpoint's port listens on loopback only (or, with -Expect closed, not at all)
#   adb-reverse   adb reverse tcp:PORT tcp:PORT (add -Remove to undo); needs the phone on USB with debugging on
#   selftest      run e2e.selftest.ps1 against canned logs
#
# The log lines the "ble_link" checks look for belong to firmware task B4b, which is not written yet. The patterns are in
# $script:Pat below, in one place: when B4b fixes its log text, change them there and in testdata\m1-dial.log. The "vox:" line
# and the "[daemon] dial ... on fw" line are the real ones (vox_probe.c, cableSession.ts).
#
# Exit codes: 0 pass, 1 fail, 2 incomplete or usage error.
param(
    [ValidateSet('m1', 'listen-check', 'adb-reverse', 'selftest')][string]$Command,
    [string]$Mac,
    [string[]]$DialLog,
    [string]$DaemonLog,
    [string]$Since,
    [int]$Port = 18481,
    [string]$OutDir,
    [string]$Serial,
    [switch]$Remove,
    [ValidateSet('loopback', 'closed')][string]$Expect = 'loopback'
)

Set-StrictMode -Version 2.0

$script:IntMinFloor = 8192
$script:Pat = @{
    CocUp    = '\bble_link: (?:channel|coc) up\b'
    LinkUp   = '\bble_link: link up #(\d+) handshake_ms=(\d+)'
    Hello    = '\[daemon\] dial (\S+) .*\bon fw\b'
    Tunnel   = 'tunnel dial (\S+) authenticated from tunnel\b'
    Vox      = '\bvox: VOX n=(\d+)\b'
    VoxDone  = '\bvox: VOX DONE\b'
    Stamp    = '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)\s'
    LocalStamp = '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\s'
}

function ConvertTo-Utc([string]$Text) {
    [datetime]::ParseExact($Text, "yyyy-MM-dd'T'HH:mm:ss.fff'Z'", [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal)
}

# harness.log (the daemon's own log) stamps lines in local time, "2026-11-01 09:18:38.888", with no zone.
function ConvertFrom-LocalStamp([string]$Text) {
    $local = [datetime]::ParseExact($Text, 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
    [TimeZoneInfo]::ConvertTimeToUtc([datetime]::SpecifyKind($local, [DateTimeKind]::Unspecified), [TimeZoneInfo]::Local)
}

# The daemon log that holds the "[cable] cable: ..." lines (NetDials and TunnelDials write there). Null when the file is absent,
# so the tunnel check is skipped and not failed.
function Get-DefaultDaemonLog([string]$HomeDir = [Environment]::GetFolderPath('UserProfile')) {
    $path = Join-Path $HomeDir '.harness\cli\data\harness.log'
    if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    $null
}

function Get-DialLogPath([string]$Mac, [datetime]$Day, [string]$LogsDir) {
    $folder = 'usb-' + ($Mac.ToUpper() -replace '[^a-zA-Z0-9_-]', '-')
    Join-Path (Join-Path $LogsDir $folder) ('dial-' + $Day.ToString('yyyyMMdd') + '.log')
}

function Get-AdbReverseArgs([int]$Port, [string]$Serial, [switch]$Remove) {
    $a = @()
    if ($Serial) { $a += @('-s', $Serial) }
    if ($Remove) { $a += @('reverse', '--remove', "tcp:$Port") }
    else { $a += @('reverse', "tcp:$Port", "tcp:$Port") }
    $a
}

function Test-LoopbackListen($Rows) {
    $rows = @($Rows)
    if ($rows.Count -eq 0) { return 'NONE' }
    foreach ($row in $rows) {
        $addr = [string]$row.LocalAddress
        if (-not ($addr -eq '::1' -or $addr -match '^127\.')) { return 'EXPOSED' }
    }
    'LOOPBACK'
}

# Lines of one or more files as @{ Time; Text }, only those at or after $Since (all when $Since is null). Lines without a stamp
# are dropped when a window is set. A missing file is reported in $Missing, never read as empty-and-fine.
function Read-Window([string[]]$Path, $Since, [ref]$Missing) {
    $out = New-Object Collections.Generic.List[object]
    foreach ($p in $Path) {
        if (-not $p -or -not (Test-Path -LiteralPath $p -PathType Leaf)) { $Missing.Value += @($p); continue }
        foreach ($line in [IO.File]::ReadAllLines($p, [Text.Encoding]::UTF8)) {
            $time = $null
            if ($line -match $script:Pat.Stamp) { $time = ConvertTo-Utc $Matches[1] }
            elseif ($line -match $script:Pat.LocalStamp) { $time = ConvertFrom-LocalStamp $Matches[1] }
            if ($null -ne $Since) {
                if ($null -eq $time -or $time -lt $Since) { continue }
            }
            $out.Add([pscustomobject]@{ Time = $time; Text = $line; File = $p })
        }
    }
    , $out.ToArray()
}

function New-Check([string]$Name, [string]$Status, [string]$Detail, $Evidence = @()) {
    [pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail; Evidence = @($Evidence) }
}

function Get-Field([string]$Line, [string]$Key) {
    if ($Line -match ('(?:^|\s)' + [regex]::Escape($Key) + '=(\S+)')) { return $Matches[1] }
    $null
}

function Test-M1 {
    param(
        [string[]]$DialLog,
        [string]$DaemonLog,
        $Since = $null,
        [string]$Mac
    )
    $checks = New-Object Collections.Generic.List[object]
    $missing = @()
    $dial = Read-Window $DialLog $Since ([ref]$missing)
    if ($missing.Count -gt 0) { $checks.Add((New-Check 'dial-log' 'FAIL' ('dial log not found: ' + ($missing -join ', ')))) }
    $daemonLines = @()
    if ($DaemonLog) {
        $dm = @()
        $daemonLines = Read-Window @($DaemonLog) $Since ([ref]$dm)
        if ($dm.Count -gt 0) { $checks.Add((New-Check 'daemon-log' 'FAIL' ('daemon log not found: ' + ($dm -join ', ')))) }
    }

    # coc-up
    $coc = @($dial | Where-Object { $_.Text -match $script:Pat.CocUp }) | Select-Object -First 1
    if ($coc) { $checks.Add((New-Check 'coc-up' 'PASS' 'the dial logged channel up' @($coc.Text))) }
    else { $checks.Add((New-Check 'coc-up' 'FAIL' 'no "ble_link: channel up" line in the window')) }

    # tls-psk-up: the first link-up at or after the channel
    $link = $null
    foreach ($l in $dial) {
        if ($l.Text -match $script:Pat.LinkUp -and ($null -eq $coc -or $l.Time -ge $coc.Time)) { $link = $l; $hs = $Matches[2]; break }
    }
    if ($link) { $checks.Add((New-Check 'tls-psk-up' 'PASS' "TLS-PSK link up, handshake_ms=$hs" @($link.Text))) }
    else { $checks.Add((New-Check 'tls-psk-up' 'FAIL' 'no "ble_link: link up #N handshake_ms=" line after the channel came up')) }

    # tunnel-session: daemon side and the dial's hello after the link
    $hello = $null
    if ($link) {
        foreach ($l in $dial) {
            if ($l.Time -ge $link.Time -and $l.Text -match $script:Pat.Hello -and (-not $Mac -or $Matches[1] -eq $Mac)) { $hello = $l; break }
        }
    }
    if (-not $DaemonLog) {
        $checks.Add((New-Check 'tunnel-session' 'SKIP' 'no -DaemonLog given: the daemon says the link is a tunnel link, and this is not checked'))
    }
    else {
        $tun = $null
        foreach ($l in $daemonLines) {
            if ($l.Text -match $script:Pat.Tunnel -and (-not $Mac -or $Matches[1] -eq $Mac)) { $tun = $l; break }
        }
        if (-not $tun) { $checks.Add((New-Check 'tunnel-session' 'FAIL' 'no "tunnel dial <MAC> authenticated from tunnel" line for this dial in the window')) }
        elseif (-not $hello) { $checks.Add((New-Check 'tunnel-session' 'FAIL' 'the daemon authenticated a tunnel dial, but the dial log shows no hello after the link came up' @($tun.Text))) }
        else { $checks.Add((New-Check 'tunnel-session' 'PASS' 'tunnel link authenticated and the dial said hello' @($tun.Text, $hello.Text))) }
    }

    # vox-turn and int-min
    $vox = @($dial | Where-Object { $_.Text -match $script:Pat.Vox -and $_.Text -notmatch '\bSKIP\b' })
    $done = @($dial | Where-Object { $_.Text -match $script:Pat.VoxDone })
    if ($vox.Count -eq 0) {
        $checks.Add((New-Check 'vox-turn' 'FAIL' 'no "vox: VOX n=" line in the window'))
        $checks.Add((New-Check 'int-min' 'FAIL' 'no VOX line, so int_min is not reported'))
    }
    else {
        $bad = @()
        foreach ($v in $vox) {
            $why = @()
            if ((Get-Field $v.Text 'codec') -ne 'adpcm') { $why += 'codec is not adpcm' }
            if ((Get-Field $v.Text 'overruns') -ne '0') { $why += 'overruns not 0' }
            if ((Get-Field $v.Text 'dropped') -ne '0') { $why += 'dropped not 0' }
            $t = Get-Field $v.Text 't_transcript_ms'
            if ($null -eq $t -or $t -notmatch '^\d+$' -or [int64]$t -le 0) { $why += 'no transcript time' }
            if ($why.Count -gt 0) { $bad += ('n=' + (Get-Field $v.Text 'VOX n') + ' ' + ($why -join ', ')) }
        }
        foreach ($d in $done) {
            $c = Get-Field $d.Text 'cum_rx_overruns'
            if ($null -ne $c -and $c -ne '0') { $bad += "VOX DONE cum_rx_overruns=$c" }
        }
        if ($bad.Count -eq 0) { $checks.Add((New-Check 'vox-turn' 'PASS' ("$($vox.Count) VOX line(s): adpcm, overruns 0, dropped 0, transcribed") @($vox | ForEach-Object { $_.Text }))) }
        else { $checks.Add((New-Check 'vox-turn' 'FAIL' ($bad -join '; ') @($vox | ForEach-Object { $_.Text }))) }

        $mins = @()
        foreach ($v in $vox) {
            $m = Get-Field $v.Text 'int_min'
            if ($null -ne $m -and $m -match '^\d+$') { $mins += [int64]$m }
        }
        if ($mins.Count -eq 0) { $checks.Add((New-Check 'int-min' 'FAIL' 'int_min is not reported on any VOX line')) }
        else {
            $lowest = ($mins | Measure-Object -Minimum).Minimum
            if ($lowest -ge $script:IntMinFloor) { $checks.Add((New-Check 'int-min' 'PASS' "int_min $lowest B, floor $($script:IntMinFloor) B")) }
            else { $checks.Add((New-Check 'int-min' 'FAIL' "int_min $lowest B is under the floor of $($script:IntMinFloor) B")) }
        }
    }

    # order
    if ($coc -and $link -and $hello -and $vox.Count -gt 0) {
        $seq = @($coc.Time, $link.Time, $hello.Time, $vox[0].Time)
        $ok = ($seq[0] -le $seq[1]) -and ($seq[1] -le $seq[2]) -and ($seq[2] -le $seq[3])
        if ($ok) { $checks.Add((New-Check 'order' 'PASS' 'channel up, link up, hello, first VOX turn, in that order')) }
        else { $checks.Add((New-Check 'order' 'FAIL' ('out of order: ' + (($seq | ForEach-Object { $_.ToString('HH:mm:ss.fff') }) -join ' > ')))) }
    }
    else { $checks.Add((New-Check 'order' 'SKIP' 'a moment is missing, so no order can be judged')) }

    $verdict = 'PASS'
    if (@($checks | Where-Object { $_.Status -eq 'SKIP' }).Count -gt 0) { $verdict = 'INCOMPLETE' }
    if (@($checks | Where-Object { $_.Status -eq 'FAIL' }).Count -gt 0) { $verdict = 'FAIL' }
    [pscustomobject]@{ Verdict = $verdict; Checks = $checks.ToArray(); DialLog = $DialLog; DaemonLog = $DaemonLog; Since = $Since }
}

function Write-Evidence($Result, [string]$Dir, [string]$Title) {
    if (-not (Test-Path -LiteralPath $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
    $path = Join-Path $Dir ("$Title-" + (Get-Date).ToString('yyyyMMdd-HHmmss') + '.txt')
    $text = New-Object Collections.Generic.List[string]
    $text.Add("$Title verdict: $($Result.Verdict)")
    $text.Add("dial log: $($Result.DialLog -join ', ')")
    $text.Add("daemon log: $($Result.DaemonLog)")
    $text.Add("since: $($Result.Since)")
    foreach ($c in $Result.Checks) {
        $text.Add('')
        $text.Add("[$($c.Status)] $($c.Name): $($c.Detail)")
        foreach ($e in $c.Evidence) { $text.Add("    $e") }
    }
    [IO.File]::WriteAllLines($path, $text, (New-Object Text.UTF8Encoding($false)))
    $path
}

# Dot-sourced (the self-test): functions only.
if ($MyInvocation.InvocationName -eq '.') { return }

function Resolve-LogsDir { Join-Path ([Environment]::GetFolderPath('UserProfile')) '.harness\logs' }

switch ($Command) {
    'm1' {
        $paths = $DialLog
        if (-not $paths) {
            if (-not $Mac) { Write-Host 'm1 needs -Mac (to find the dial log) or -DialLog'; exit 2 }
            $paths = @(Get-DialLogPath -Mac $Mac -Day (Get-Date) -LogsDir (Resolve-LogsDir))
        }
        $from = $null
        if ($Since) { $from = [datetime]::Parse($Since, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal) }
        # Default: the daemon's own log, when it exists. An explicit -DaemonLog that is missing is a failure, not a skip.
        if (-not $DaemonLog) { $DaemonLog = Get-DefaultDaemonLog }
        $result = Test-M1 -DialLog $paths -DaemonLog $DaemonLog -Since $from -Mac $Mac
        foreach ($c in $result.Checks) { Write-Host ('{0,-10} {1,-15} {2}' -f $c.Status, $c.Name, $c.Detail) }
        if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) 'phone-bridge-e2e' }
        $evidence = Write-Evidence $result $OutDir 'm1'
        Write-Host "verdict: $($result.Verdict)"
        Write-Host "evidence: $evidence"
        if ($result.Verdict -eq 'PASS') { exit 0 }
        if ($result.Verdict -eq 'FAIL') { exit 1 }
        exit 2
    }
    'listen-check' {
        $rows = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
        $state = Test-LoopbackListen -Rows $rows
        Write-Host ("port {0}: {1} ({2})" -f $Port, $state, (($rows | ForEach-Object { $_.LocalAddress }) -join ', '))
        if ($Expect -eq 'closed') { if ($state -eq 'NONE') { exit 0 } else { exit 1 } }
        if ($state -eq 'LOOPBACK') { exit 0 }
        exit 1
    }
    'adb-reverse' {
        $adb = Get-Command adb -ErrorAction SilentlyContinue
        if (-not $adb) { Write-Host 'adb is not on PATH'; exit 2 }
        $argv = Get-AdbReverseArgs -Port $Port -Serial $Serial -Remove:$Remove
        Write-Host ('adb ' + ($argv -join ' '))
        & $adb.Source @argv
        $code = $LASTEXITCODE
        $list = @()
        if ($Serial) { $list += @('-s', $Serial) }
        & $adb.Source @list reverse --list
        exit $code
    }
    'selftest' { & (Join-Path $PSScriptRoot 'e2e.selftest.ps1'); exit $LASTEXITCODE }
    default { Write-Host 'usage: e2e.ps1 -Command m1|listen-check|adb-reverse|selftest  (see the header of this file)'; exit 2 }
}
