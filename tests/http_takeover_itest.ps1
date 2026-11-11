<#
  102 - two-process test for the port handover (tests/http_takeover_itest.c).
  Builds the test program with MSVC and runs seven scenarios on the TEST ports 47834 / 47835
  (never 52734: a running TeamSpeak with the plugin is not touched).
  Usage:  pwsh -File tests\http_takeover_itest.ps1        exit code 0 = all scenarios passed
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'tests\out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'http_takeover_itest.exe'
$P1 = 47834; $P2 = 47835
$script:failed = 0

function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  OK   $name" } else { Write-Host "  FAIL $name"; $script:failed++ }
}

# ---- build ---------------------------------------------------------------
# Same search as build.ps1: known install folders first, vswhere as a fallback.
$vcvars = $null
foreach ($edition in '18\Community', '18\BuildTools', '18\Professional', '18\Enterprise',
                     '2022\Community', '2022\BuildTools', '2022\Professional', '2022\Enterprise') {
    $c = Join-Path $env:ProgramFiles "Microsoft Visual Studio\$edition\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $c) { $vcvars = $c; break }
}
if (-not $vcvars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
        if ($vs) { $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
    }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) { throw 'Visual Studio with the C++ tools not found' }
$cl = "cl /nologo /W4 /WX /O2 /I. /Isrc /D_CRT_SECURE_NO_WARNINGS /DPOS_HTTP_PORT_LIST=`"{$P1,$P2}`" tests\http_takeover_itest.c src\core\http\pos_http_server.c /Fo:tests\out\ /Fe:tests\out\http_takeover_itest.exe ws2_32.lib iphlpapi.lib user32.lib"
Push-Location $root
try {
    $build = cmd /c "`"$vcvars`" >nul 2>&1 && $cl" 2>&1
} finally { Pop-Location }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { $build | Select-Object -Last 25 | ForEach-Object { Write-Host $_ }; throw 'build of the test program failed' }
Write-Host 'build ok'

# ---- helpers -------------------------------------------------------------
function Start-Inst([string]$label, [int]$active, [int]$seconds, [int]$activeAfter = -1, [int]$restartAfter = -1) {
    $log = Join-Path $out "itest_$label.txt"
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $a = @($label, "$active", "$seconds", "$activeAfter", "$restartAfter")
    $p = Start-Process -FilePath $exe -ArgumentList $a -RedirectStandardOutput $log -WindowStyle Hidden -PassThru
    return [pscustomobject]@{ Proc = $p; Log = $log; Label = $label }
}
# A program with the SAME executable that keeps a port and never answers: what a frozen TeamSpeak looks like.
function Start-Hold([string]$label, [int]$port, [int]$seconds) {
    $log = Join-Path $out "itest_$label.txt"
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -ArgumentList @('HOLD', "$port", "$seconds") -RedirectStandardOutput $log -WindowStyle Hidden -PassThru
    $inst = [pscustomobject]@{ Proc = $p; Log = $log; Label = $label }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 3000 -and (Hits 'HOLD PORT' $inst) -eq 0) { Start-Sleep -Milliseconds 100 }
    return $inst
}
function Lines($inst) { if (Test-Path $inst.Log) { @(Get-Content -LiteralPath $inst.Log) } else { @() } }
# Number of output lines of the given instances that contain the text.
function Hits([string]$text, $instances) {
    $n = 0
    foreach ($i in @($instances)) { $n += @(Lines $i | Where-Object { $_.Contains($text) }).Count }
    return $n
}
function Last-Port($inst) {
    $l = Lines $inst | Where-Object { $_ -match " PORT (\d+)$" } | Select-Object -Last 1
    if ($l -and $l -match " PORT (\d+)$") { return [int]$Matches[1] } else { return -1 }
}
function Wait-Port($inst, [int]$port, [int]$ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) { if ((Last-Port $inst) -eq $port) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function Http([string]$method, [int]$port, [string]$path, [string]$body = $null) {
    try {
        $p = @{ Uri = "http://127.0.0.1:$port$path"; Method = $method; TimeoutSec = 3; UseBasicParsing = $true }
        if ($body) { $p.Body = $body; $p.ContentType = 'application/json' }
        return (Invoke-WebRequest @p).Content
    } catch { return "ERR $($_.Exception.Message)" }
}
function Stop-All($list) { foreach ($i in $list) { if ($i -and -not $i.Proc.HasExited) { $i.Proc.Kill(); $i.Proc.WaitForExit(3000) | Out-Null } } }
function Foreign([int]$port) { $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port); $l.Start(); return $l }
$POS = '{"seq":1,"x":1000.0,"y":2000.0,"z":300.0,"yaw":10.0}'

# ---- scenarios -----------------------------------------------------------
Write-Host '[1] one idle instance'
$a = Start-Inst 'A1' 0 6
Check (Wait-Port $a $P1) 'binds the primary port'
$s = Http GET $P1 '/v1/status'
Check ($s -match '"service":"conan_exiles_ts"' -and $s -match '"act":0' -and $s -match "`"port`":$P1") "status: our service, idle, port ($s)"
$r = Http POST $P1 '/v1/position' $POS
Check ($r -match '"ok":true' -and $r -match ',"act":0}$') "position reply ends with act 0 ($r)"
Check ((Http GET $P1 '/health') -match "127\.0\.0\.1:$P1") 'health names the bound port'
Stop-All @($a)

Write-Host '[2] idle holder, second idle instance: the newcomer gets the primary port'
$a = Start-Inst 'A2' 0 9; [void](Wait-Port $a $P1)
$b = Start-Inst 'B2' 0 7
Check (Wait-Port $b $P1 5000) 'newcomer ends on the primary port'
Check (Wait-Port $a $P2 5000) 'old idle holder moved to the fallback port'
Check ((Hits 'handing it over' $a) -ge 1) 'holder logged the handover'
Check ((Hits ' NOTICE ' @($a, $b)) -eq 0) 'no user notice for a clean handover'
Stop-All @($a, $b)

Write-Host '[3] active holder keeps the port'
$a = Start-Inst 'A3' 1 8; [void](Wait-Port $a $P1)
$b = Start-Inst 'B3' 0 6
Check (Wait-Port $b $P2 5000) 'newcomer takes the fallback port'
Start-Sleep -Milliseconds 300
Check ((Last-Port $a) -eq $P1) 'active holder still on the primary port'
Check ((Http POST $P1 '/v1/position' $POS) -match ',"act":1}$') 'primary answers act 1'
Check ((Http GET $P2 '/v1/status') -match '"act":0') 'fallback instance says act 0'
Check ((Hits ' NOTICE ' $b) -eq 0) 'no notice: an active instance of ours serves the primary'
Stop-All @($a, $b)

Write-Host '[4] idle holder, ACTIVE newcomer: newcomer takes over and answers act 1'
$a = Start-Inst 'A4' 0 9; [void](Wait-Port $a $P1)
$b = Start-Inst 'B4' 1 7
Check (Wait-Port $b $P1 5000) 'active newcomer on the primary port'
Check ((Http POST $P1 '/v1/position' $POS) -match ',"act":1}$') 'the mod now reaches the active instance'
Stop-All @($a, $b)

Write-Host '[5] primary held by a foreign program that never answers'
$f = Foreign $P1
try {
    $b = Start-Inst 'B5' 1 16
    Check (Wait-Port $b $P2 5000) 'instance takes the fallback port'
    Start-Sleep -Milliseconds 300
    Check ((Hits ' NOTICE 1 ' $b) -ge 1) 'notice 1 (fallback) published'
    Check ((Hits 'does not answer' $b) -ge 1) 'log says the holder does not answer'
    $f.Stop(); $f = $null
    Check (Wait-Port $b $P1 9000) 'moves back to the primary port once it is free'
} finally { if ($f) { $f.Stop() }; Stop-All @($b) }

Write-Host '[6] both ports held by foreign programs'
$f1 = Foreign $P1; $f2 = Foreign $P2
try {
    $b = Start-Inst 'B6' 1 6
    Start-Sleep -Seconds 3
    Check ((Last-Port $b) -le 0) 'no port bound'
    Check ((Hits ' NOTICE 2 ' $b) -ge 1) 'notice 2 (no port) published'
} finally { $f1.Stop(); $f2.Stop(); Stop-All @($b) }

Write-Host '[7] the instance on the fallback port becomes active later: it gets the primary port back from the idle one'
# Two idle TeamSpeak: the second takes the primary port, the first moves to the fallback port.
# Then the user connects the FIRST one to the Conan server. It must end up on the primary port again,
# because the game mod (today) only knows that port.
$a = Start-Inst 'A7' 0 30 4; [void](Wait-Port $a $P1)
$b = Start-Inst 'B7' 0 28
Check (Wait-Port $b $P1 5000) 'newcomer got the primary port first'
Check (Wait-Port $a $P2 5000) 'first instance on the fallback port'
Check (Wait-Port $a $P1 14000) 'after becoming active the first instance is back on the primary port'
Check (Wait-Port $b $P2 5000) 'the idle instance moved to the fallback port'
Check ((Http POST $P1 '/v1/position' $POS) -match ',"act":1}$') 'the mod reaches the active instance on the primary port'
Start-Sleep -Seconds 7
Check ((Last-Port $a) -eq $P1 -and (Last-Port $b) -eq $P2) 'stable afterwards: the idle instance does not take the port back'
Stop-All @($a, $b)

Write-Host '[8] restart right after traffic: the port must be free at once (plugin restart inside TeamSpeak)'
# The game mod opens one connection per request (33 per second). The listener closes first, which leaves
# connections in TIME_WAIT on the port. A new listener must still be able to bind it immediately.
function Post-Raw([int]$port, [string]$body) {
    $c = [System.Net.Sockets.TcpClient]::new()
    try {
        $c.Connect('127.0.0.1', $port)
        $s = $c.GetStream()
        $req = "POST /v1/position HTTP/1.1`r`nHost: 127.0.0.1:$port`r`nContent-Type: application/json`r`nContent-Length: $($body.Length)`r`n`r`n$body"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $s.Write($bytes, 0, $bytes.Length)
        $buf = New-Object byte[] 2048
        $n = $s.Read($buf, 0, $buf.Length)
        return [Text.Encoding]::ASCII.GetString($buf, 0, $n)
    } catch { return "ERR $($_.Exception.Message)" } finally { $c.Close() }
}
$a = Start-Inst 'A8' 1 30; [void](Wait-Port $a $P1)
$okCount = 0
for ($i = 0; $i -lt 200; $i++) { if ((Post-Raw $P1 $POS) -match '"ok":true') { $okCount++ } }
Check ($okCount -eq 200) "200 requests answered ($okCount)"
Stop-All @($a)
$b = Start-Inst 'B8' 1 8
Check (Wait-Port $b $P1 3000) 'a new instance binds the primary port right after the old one ended'
Check ((Hits ' NOTICE ' $b) -eq 0) 'no notice'
Stop-All @($b)

Write-Host '[9] handover right after traffic: the idle holder served 200 requests, then an active instance asks for the port'
$a = Start-Inst 'A9' 0 30; [void](Wait-Port $a $P1)
$okCount = 0
for ($i = 0; $i -lt 200; $i++) { if ((Post-Raw $P1 $POS) -match '"ok":true') { $okCount++ } }
Check ($okCount -eq 200) "200 requests answered by the idle holder ($okCount)"
$b = Start-Inst 'B9' 1 12
Check (Wait-Port $b $P1 6000) 'the active newcomer gets the primary port'
Check (Wait-Port $a $P2 6000) 'the old holder is on the fallback port'
Check ((Hits ' NOTICE ' @($a, $b)) -eq 0) 'no notice'
Stop-All @($a, $b)

Write-Host '[10] plugin disable + enable inside one process right after traffic'
$a = Start-Inst 'A10' 1 30 -1 6; [void](Wait-Port $a $P1)
$okCount = 0
for ($i = 0; $i -lt 150; $i++) { if ((Post-Raw $P1 $POS) -match '"ok":true') { $okCount++ } }
Check ($okCount -eq 150) "150 requests answered before the restart ($okCount)"
$sw10 = [Diagnostics.Stopwatch]::StartNew()
while ($sw10.ElapsedMilliseconds -lt 12000 -and (Hits ' RESTART' $a) -eq 0) { Start-Sleep -Milliseconds 100 }
Check ((Hits ' RESTART' $a) -eq 1) 'the instance restarted its listener'
Start-Sleep -Seconds 2
$after = @(Lines $a); $idx = [array]::IndexOf($after, ($after | Where-Object { $_ -like '* RESTART' } | Select-Object -First 1))
$tail = if ($idx -ge 0) { $after[($idx + 1)..($after.Count - 1)] } else { @() }
Check (@($tail | Where-Object { $_ -like "* PORT $P1" }).Count -ge 1) 'after the restart it listens on the primary port again'
Check (@($tail | Where-Object { $_ -like '*fallback*' -or $_ -like '* NOTICE *' }).Count -eq 0) 'no fallback and no notice after the restart'
Check ((Http POST $P1 '/v1/position' $POS) -match ',"act":1}$') 'and answers requests (act 1 set again after the restart)'
Stop-All @($a)

Write-Host '[11] a frozen TeamSpeak (same program, holds the port, never answers) and an ACTIVE instance: the frozen one is ended'
$h = Start-Hold 'H11' $P1 90
Check ((Hits 'HOLD PORT' $h) -eq 1) 'the frozen stand-in holds the primary port'
$b = Start-Inst 'B11' 1 40
Check (Wait-Port $b $P2 5000) 'the active instance starts on the fallback port (nothing is ended at once)'
Start-Sleep -Seconds 4
Check (-not $h.Proc.HasExited) 'after 4 s the frozen process is still there (three tries over at least 8 s are needed)'
Check (Wait-Port $b $P1 25000) 'then the active instance is on the primary port'
Check ($h.Proc.WaitForExit(3000)) 'the frozen process was ended'
Check ((Hits 'ended the frozen TeamSpeak' $b) -eq 1) 'the log says so'
Check ((Hits ' ENDED ' $b) -eq 1) 'the chat notice is published once'
Stop-All @($b, $h)

Write-Host '[12] the same frozen stand-in, but the instance is NOT active: nothing is ended'
$h = Start-Hold 'H12' $P1 90
$b = Start-Inst 'B12' 0 30
Check (Wait-Port $b $P2 5000) 'the idle instance takes the fallback port'
Start-Sleep -Seconds 16
Check (-not $h.Proc.HasExited) 'after 16 s the frozen process is still alive'
Check ((Last-Port $b) -eq $P2) 'the idle instance stays on the fallback port'
Check ((Hits 'ended the frozen' $b) -eq 0 -and (Hits ' ENDED ' $b) -eq 0) 'nothing was ended'
Stop-All @($b, $h)

Write-Host '[13] a foreign program that never answers and an ACTIVE instance for 16 s: the foreign program is never ended'
$f = Foreign $P1
try {
    $b = Start-Inst 'B13' 1 24
    Check (Wait-Port $b $P2 5000) 'the active instance takes the fallback port'
    Start-Sleep -Seconds 16
    Check ((Last-Port $b) -eq $P2) 'it stays there'
    Check ((Hits 'ended the frozen' $b) -eq 0 -and (Hits ' ENDED ' $b) -eq 0) 'nothing was ended (this test script is the foreign program and still runs)'
} finally { $f.Stop(); Stop-All @($b) }

if ($script:failed) { Write-Host "FAILED: $($script:failed) check(s)"; exit 1 }
Write-Host 'all scenarios passed'
exit 0
