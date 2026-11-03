# Self-test of e2e.ps1 against canned logs in testdata\ (no adb, no network, no daemon, no serial port).
# Run: powershell -NoProfile -File e2e.selftest.ps1   (exit 0 when every case passes)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e.ps1')

$script:failed = 0
$script:passed = 0
function Assert-Equal($Actual, $Expected, [string]$What) {
    if ("$Actual" -eq "$Expected") { $script:passed++; Write-Host "  ok   $What" }
    else { $script:failed++; Write-Host "  FAIL $What : expected '$Expected', got '$Actual'" }
}

$data = Join-Path $PSScriptRoot 'testdata'
$dialFile = Join-Path $data 'm1-dial.log'
$daemonFile = Join-Path $data 'm1-daemon.log'
$t0 = [datetime]::Parse('2026-11-01T18:00:00Z', [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal)
$dialMac = 'B8:1F:3F:CA:E5:58'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('e2e-selftest-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Status($result, [string]$name) { ($result.Checks | Where-Object { $_.Name -eq $name }).Status }
function Mutate([string]$from, [string]$old, [string]$new, [string]$leaf) {
    $text = [IO.File]::ReadAllText($from, [Text.Encoding]::UTF8)
    if (-not $text.Contains($old)) { throw "test setup: '$old' not in $from" }
    $path = Join-Path $tmp $leaf
    [IO.File]::WriteAllText($path, $text.Replace($old, $new), (New-Object Text.UTF8Encoding($false)))
    $path
}

Write-Host 'M1 on a good session'
$r = Test-M1 -DialLog $dialFile -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal $r.Verdict 'PASS' 'verdict'
foreach ($n in 'coc-up', 'tls-psk-up', 'tunnel-session', 'vox-turn', 'int-min', 'order') { Assert-Equal (Status $r $n) 'PASS' "check $n" }
Assert-Equal (@(($r.Checks | Where-Object { $_.Name -eq 'vox-turn' }).Evidence).Count) 1 'only the VOX line inside the window counts (the 17:59 one is older)'

Write-Host 'M1 without -Since reads the whole file, and the old bad VOX line fails it'
$r = Test-M1 -DialLog $dialFile -DaemonLog $daemonFile -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'old overruns/pcm line is seen'

Write-Host 'M1 when the daemon log is not given'
$r = Test-M1 -DialLog $dialFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'SKIP' 'tunnel check is skipped, not passed'
Assert-Equal $r.Verdict 'INCOMPLETE' 'verdict'

Write-Host 'M1 when the tunnel line is for another dial, too old, or not a tunnel'
$r = Test-M1 -DialLog $dialFile -DaemonLog $daemonFile -Since ($t0.AddMinutes(5)) -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'FAIL' 'nothing after the window start'
$other = Mutate $daemonFile 'E5:58 authenticated' 'E5:99 authenticated' 'other.log'
$r = Test-M1 -DialLog $dialFile -DaemonLog $other -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'FAIL' 'another MAC'
$wifi = Mutate $daemonFile 'tunnel dial B8:1F:3F:CA:E5:58 authenticated from tunnel' 'Wi-Fi dial B8:1F:3F:CA:E5:58 authenticated from 192.168.1.40' 'wifi.log'
$r = Test-M1 -DialLog $dialFile -DaemonLog $wifi -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'FAIL' 'a Wi-Fi link is not a tunnel link'

Write-Host 'M1 failure cases'
$p = Mutate $dialFile 'channel up' 'channel down' 'nococ.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'coc-up') 'FAIL' 'no channel-up line'
Assert-Equal $r.Verdict 'FAIL' 'verdict'
$p = Mutate $dialFile 'link up #1' 'link dn #1' 'notls.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tls-psk-up') 'FAIL' 'no link-up line'
$p = Mutate $dialFile 'codec=adpcm' 'codec=pcm' 'pcm.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'PCM over BLE'
$p = Mutate $dialFile 'overruns=0 dropped=0 sent_bytes=128000 codec=adpcm' 'overruns=2 dropped=0 sent_bytes=128000 codec=adpcm' 'overrun.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'overruns 2'
$p = Mutate $dialFile 'dropped=0 sent_bytes=128000 codec=adpcm' 'dropped=5 sent_bytes=128000 codec=adpcm' 'dropped.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'dropped 5'
$p = Mutate $dialFile 't_transcript_ms=2352' 't_transcript_ms=-1' 'notranscript.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'no transcript time'
$p = Mutate $dialFile 'DONE utterances=1 cum_rx_overruns=0' 'DONE utterances=1 cum_rx_overruns=4' 'cumoverrun.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'vox-turn') 'FAIL' 'cumulative rx overruns 4 on the DONE line'
$p = Mutate $dialFile 'int_min=139688' 'int_min=8191' 'lowmin.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'int-min') 'FAIL' 'int_min 8191 is under the 8192 floor'
$p = Mutate $dialFile 'int_min=139688' 'int_min=8192' 'edgemin.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'int-min') 'PASS' 'int_min 8192 is on the floor'
$p = Mutate $dialFile ' int_min=139688' '' 'nomin.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'int-min') 'FAIL' 'int_min not reported'
$p = Mutate $dialFile '2026-11-01T18:00:30.000Z' '2026-11-01T18:00:02.000Z' 'early.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'order') 'FAIL' 'VOX before the link was up'
$p = Mutate $dialFile '[daemon] dial B8:1F:3F:CA:E5:58 on fw' '[daemon] nope B8:1F:3F:CA:E5:58 on fw' 'nohello.log'
$r = Test-M1 -DialLog $p -DaemonLog $daemonFile -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'FAIL' 'no hello from the dial after the link came up'

Write-Host 'M1 with the daemon log as the daemon writes it (harness.log: local-time stamps, no Z)'
function LocalStamp([datetime]$utc) { $utc.ToLocalTime().ToString('yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture) }
$localLog = Join-Path $tmp 'harness.log'
$lines = @(
    ((LocalStamp ($t0.AddMinutes(-5))) + ' [cable] cable: tunnel dial B8:1F:3F:CA:E5:58 authenticated from tunnel'),
    ((LocalStamp ($t0.AddSeconds(3.65))) + ' [cable] cable: Wi-Fi dial B8:1F:3F:CA:E5:50 authenticated from 192.168.10.166'),
    ((LocalStamp ($t0.AddSeconds(3.65))) + ' [cable] cable: tunnel dial B8:1F:3F:CA:E5:58 authenticated from tunnel')
)
[IO.File]::WriteAllLines($localLog, $lines, (New-Object Text.UTF8Encoding($false)))
$r = Test-M1 -DialLog $dialFile -DaemonLog $localLog -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'PASS' 'a local-time line inside the window is found (and the one 5 minutes older is ignored)'
$onlyOld = Join-Path $tmp 'harness-old.log'
[IO.File]::WriteAllLines($onlyOld, @($lines[0]), (New-Object Text.UTF8Encoding($false)))
$r = Test-M1 -DialLog $dialFile -DaemonLog $onlyOld -Since $t0 -Mac $dialMac
Assert-Equal (Status $r 'tunnel-session') 'FAIL' 'only a line before the window: no tunnel session'

Write-Host 'A log the daemon still holds open for writing (the real harness.log is)'
$held = [IO.File]::Open($localLog, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
try {
    $r = Test-M1 -DialLog $dialFile -DaemonLog $localLog -Since $t0 -Mac $dialMac
    Assert-Equal (Status $r 'tunnel-session') 'PASS' 'read while a writer holds the file'
}
finally { $held.Dispose() }

Write-Host 'Default daemon log path'
$fakeHome = Join-Path $tmp 'home'
Assert-Equal ([bool](Get-DefaultDaemonLog -HomeDir $fakeHome)) $false 'absent file: no default (the check is skipped, not failed)'
New-Item -ItemType Directory -Path (Join-Path $fakeHome '.harness\cli\data') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $fakeHome '.harness\cli\data\harness.log'), '')
Assert-Equal (Get-DefaultDaemonLog -HomeDir $fakeHome) (Join-Path $fakeHome '.harness\cli\data\harness.log') 'present file: its path'

Write-Host 'M1 with a missing file'
$r = Test-M1 -DialLog (Join-Path $tmp 'absent.log') -Since $t0 -Mac $dialMac
Assert-Equal $r.Verdict 'FAIL' 'missing dial log is a failure, not a pass'

Write-Host 'Dial log path'
$d = Get-DialLogPath -Mac 'b8:1f:3f:ca:e5:58' -Day ([datetime]'2026-11-01') -LogsDir 'C:\logs'
Assert-Equal $d 'C:\logs\usb-B8-1F-3F-CA-E5-58\dial-20261101.log' 'folder and file name match the daemon'

Write-Host 'Loopback-only listener check'
$rows = @([pscustomobject]@{ LocalAddress = '127.0.0.1'; LocalPort = 18481 })
Assert-Equal (Test-LoopbackListen -Rows $rows) 'LOOPBACK' '127.0.0.1 only'
$rows = @([pscustomobject]@{ LocalAddress = '0.0.0.0'; LocalPort = 18481 })
Assert-Equal (Test-LoopbackListen -Rows $rows) 'EXPOSED' '0.0.0.0'
$rows = @([pscustomobject]@{ LocalAddress = '127.0.0.1'; LocalPort = 18481 }, [pscustomobject]@{ LocalAddress = '192.168.1.20'; LocalPort = 18481 })
Assert-Equal (Test-LoopbackListen -Rows $rows) 'EXPOSED' 'loopback plus a LAN address'
$rows = @([pscustomobject]@{ LocalAddress = '::'; LocalPort = 18481 })
Assert-Equal (Test-LoopbackListen -Rows $rows) 'EXPOSED' '::'
$rows = @([pscustomobject]@{ LocalAddress = '::1'; LocalPort = 18481 })
Assert-Equal (Test-LoopbackListen -Rows $rows) 'LOOPBACK' '::1'
Assert-Equal (Test-LoopbackListen -Rows @()) 'NONE' 'nothing listening'

Write-Host 'adb argument lists'
Assert-Equal ((Get-AdbReverseArgs -Port 18481) -join ' ') 'reverse tcp:18481 tcp:18481' 'reverse'
Assert-Equal ((Get-AdbReverseArgs -Port 18481 -Remove) -join ' ') 'reverse --remove tcp:18481' 'remove'
Assert-Equal ((Get-AdbReverseArgs -Port 18481 -Serial 'ABC123') -join ' ') '-s ABC123 reverse tcp:18481 tcp:18481' 'serial'

Remove-Item -Recurse -Force $tmp
Write-Host ''
Write-Host "passed $script:passed, failed $script:failed"
if ($script:failed -gt 0) { exit 1 }
exit 0
