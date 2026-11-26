param(
    [int]$DelayMs = 300,     # how late the slow answers come
    [int]$EveryMs = 3000,    # one slow answer this often (0 = every answer is slow)
    [int]$Port = 52734,      # the plugin's port - close TeamSpeak first
    [int]$Minutes = 10       # stops by itself after this time
)
# Stand-in for the TeamSpeak plugin (doku 106): answers the game mod's
#   POST /v1/position   with {"ok":true,"talk":0,"talkers":"","act":1}
#   GET  /v1/status     with a status line (act 1)
# and every $EveryMs ms delays one answer by $DelayMs. Like the real plugin it
# serves one request after the other, so a late answer also holds back the
# requests behind it. Question it answers: does the GAME stutter when the
# answer is late (the game waits), or does it stay smooth (it does not wait)?
# Usage (TeamSpeak closed, then start the game and play):
#   powershell -ExecutionPolicy Bypass -File tests\slow_reply_standin.ps1 -DelayMs 300 -EveryMs 3000
$ErrorActionPreference = 'Stop'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
try { $listener.Start() } catch { Write-Host "Port $Port is taken - close TeamSpeak (all ts3client_win64.exe) first."; exit 1 }
Write-Host ("Stand-in on 127.0.0.1:{0}: one answer {1} ms late every {2} ms, stops after {3} min (Ctrl+C stops earlier)." -f $Port, $DelayMs, $EveryMs, $Minutes)
$okBody = '{"ok":true,"talk":0,"talkers":"","act":1}'
$statusBody = '{"ok":true,"service":"conan_exiles_ts","act":1,"pid":' + $PID + ',"port":' + $Port + ',"ver":"standin"}'
$sw = [Diagnostics.Stopwatch]::StartNew()
$nextSlow = $EveryMs
$count = 0; $slow = 0; $lastReport = 0
try {
    while ($sw.Elapsed.TotalMinutes -lt $Minutes) {
        if (-not $listener.Pending()) { Start-Sleep -Milliseconds 1; continue }
        $client = $listener.AcceptTcpClient()
        try {
            $client.ReceiveTimeout = 500
            $s = $client.GetStream()
            $buf = New-Object byte[] 8192; $got = 0; $headerEnd = -1
            while ($got -lt $buf.Length) {
                $n = $s.Read($buf, $got, $buf.Length - $got)
                if ($n -le 0) { break }
                $got += $n
                $text = [Text.Encoding]::ASCII.GetString($buf, 0, $got)
                $headerEnd = $text.IndexOf("`r`n`r`n")
                if ($headerEnd -ge 0) {
                    $cl = 0
                    if ($text -match '(?im)^Content-Length:\s*(\d+)') { $cl = [int]$Matches[1] }
                    if ($got -ge $headerEnd + 4 + $cl) { break }
                }
            }
            $text = [Text.Encoding]::ASCII.GetString($buf, 0, $got)
            $body = if ($text -match '^GET /v1/status') { $statusBody } else { $okBody }
            $late = $false
            if ($EveryMs -le 0 -or $sw.ElapsedMilliseconds -ge $nextSlow) {
                Start-Sleep -Milliseconds $DelayMs
                $nextSlow = $sw.ElapsedMilliseconds + $EveryMs
                $late = $true; $slow++
            }
            $bytes = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
            $s.Write($bytes, 0, $bytes.Length)
            $count++
            if ($late) { Write-Host ("{0:HH:mm:ss.fff}  answer #{1} sent {2} ms late" -f (Get-Date), $count, $DelayMs) }
        } catch { } finally { $client.Close() }
        if ($sw.Elapsed.TotalSeconds - $lastReport -ge 30) { $lastReport = $sw.Elapsed.TotalSeconds; Write-Host ("{0:HH:mm:ss}  {1} answers so far, {2} of them late" -f (Get-Date), $count, $slow) }
    }
} finally {
    $listener.Stop()
    Write-Host ("Stopped: {0} answers, {1} late." -f $count, $slow)
}
