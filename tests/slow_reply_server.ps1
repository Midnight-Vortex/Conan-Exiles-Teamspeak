<#
  105 - stand-in for the plugin that answers SLOWLY on purpose (manual test, UNTESTED - see doku 105).

  Question it answers: does the GAME stutter when the reply to its position request is late?
  Close TeamSpeak (it holds port 52734), start this script, then play. The mod posts its position ~33 times a
  second to http://127.0.0.1:<Port>/v1/position; this server answers every request correctly, but now and then
  it holds ONE position reply back for -DelayMs. If the game freezes for about that long every time the script
  prints a "slow answer" line, a late reply alone is enough to freeze the game - the plugin only has to be
  fast (the plugin writes "HTTP: slow request ..." into plugin.log when it is not).

  Like the plugin it serves one connection after another (accept, read, answer, close), so a held-back reply
  also delays the next request.

  Answers:   POST /v1/position  200 {"ok":true,"talk":0,"talkers":"","act":1}   (only these can be slow)
             GET  /v1/status    200 {"ok":true,"service":"conan_exiles_ts","act":1,"pid":0,"port":<Port>,"ver":"test"}
             everything else    404
  Windows PowerShell 5.1, no modules. Stop with Ctrl+C.

  Usage:  powershell -ExecutionPolicy Bypass -File tests\slow_reply_server.ps1 [-Port 52734] [-DelayMs 300] [-EveryMs 3000]
    -Port     TCP port on 127.0.0.1 (default 52734, the plugin's primary port)
    -DelayMs  how long a slow position reply is held back (default 300)
    -EveryMs  one slow position reply every N ms (default 3000); 0 = EVERY position reply is slow
#>
param(
    [int]$Port = 52734,
    [int]$DelayMs = 300,
    [int]$EveryMs = 3000
)
$ErrorActionPreference = 'Stop'
if ($Port -lt 1 -or $Port -gt 65535) { throw '-Port must be 1..65535' }
if ($DelayMs -lt 0) { throw '-DelayMs must be 0 or more' }
if ($EveryMs -lt 0) { throw '-EveryMs must be 0 or more' }

$ReadTimeoutMs = 1000      # one read waits at most this long: a silent client cannot hang the server
$RequestDeadlineMs = 2000  # the whole request (headers + body) must be in within this time
$MaxHeaderBytes = 16384
$MaxBodyBytes = 65536
$StatsEveryMs = 5000

# ---- helpers ---------------------------------------------------------------

# Reads one request (headers, then Content-Length bytes of body) from the stream.
# Returns @{ Method; Path } or $null when the client was silent, too slow, sent garbage or went away.
function Read-HttpRequest($stream) {
    $buf = New-Object byte[] 4096
    $all = New-Object System.IO.MemoryStream
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $headEnd = -1
    $need = 0L
    $head = ''
    while ($true) {
        if ($watch.ElapsedMilliseconds -ge $RequestDeadlineMs) { return $null }
        try { $n = $stream.Read($buf, 0, $buf.Length) } catch { return $null }   # read timeout / reset
        if ($n -le 0) { return $null }                                           # client closed
        $all.Write($buf, 0, $n)

        if ($headEnd -lt 0) {
            # ASCII decoding keeps one char per byte, so a string index is also a byte index.
            $text = [System.Text.Encoding]::ASCII.GetString($all.GetBuffer(), 0, [int]$all.Length)
            $i = $text.IndexOf("`r`n`r`n", [System.StringComparison]::Ordinal)
            if ($i -lt 0) {
                if ($all.Length -gt $MaxHeaderBytes) { return $null }
                continue
            }
            $headEnd = $i + 4
            $head = $text.Substring(0, $i)
            $m = [regex]::Match($head, '(?im)^Content-Length:\s*(\d+)\s*$')
            if ($m.Success) {
                $v = 0L
                if (-not [long]::TryParse($m.Groups[1].Value, [ref]$v)) { return $null }
                if ($v -gt $MaxBodyBytes) { return $null }
                $need = $v
            }
        }
        if (($all.Length - $headEnd) -ge $need) { break }                        # body complete
    }

    $first = ($head -split "`n")[0].TrimEnd("`r")
    $parts = $first -split ' '
    if ($parts.Count -lt 2) { return $null }
    return [pscustomobject]@{ Method = $parts[0]; Path = ($parts[1] -split '\?')[0] }
}

# Sends one complete JSON answer. The caller closes the connection ("Connection: close").
function Send-Json($stream, [int]$status, [string]$body) {
    $reason = 'Not Found'
    if ($status -eq 200) { $reason = 'OK' }
    $text = "HTTP/1.1 $status $reason`r`nContent-Type: application/json`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body"
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($text)   # body is plain ASCII, so Length == byte count
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

function Stamp { return (Get-Date -Format 'HH:mm:ss.fff') }

# ---- server ----------------------------------------------------------------

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
try {
    $listener.Start()
} catch {
    throw "cannot listen on 127.0.0.1:$Port - is TeamSpeak with the plugin (or another copy of this script) still running? $($_.Exception.Message)"
}

$statusBody = '{"ok":true,"service":"conan_exiles_ts","act":1,"pid":0,"port":' + $Port + ',"ver":"test"}'
$positionBody = '{"ok":true,"talk":0,"talkers":"","act":1}'

$clock = [System.Diagnostics.Stopwatch]::StartNew()
$lastSlowAt = $clock.ElapsedMilliseconds
$lastStatsAt = $clock.ElapsedMilliseconds
$requests = 0
$positions = 0
$statuses = 0
$others = 0
$slow = 0
$dropped = 0
$errors = 0
$intervalRequests = 0

Write-Host ("[{0}] listening on http://127.0.0.1:{1}  (slow position reply: {2} ms, {3}). Ctrl+C stops." -f
    (Stamp), $Port, $DelayMs, $(if ($EveryMs -eq 0) { 'every reply' } else { "one every $EveryMs ms" }))

try {
    $accept = $listener.AcceptTcpClientAsync()
    while ($true) {
        # Wait for a client, but wake up every 200 ms so Ctrl+C and the counter line are never stuck behind accept().
        if (-not $accept.Wait(200)) {
            if (($clock.ElapsedMilliseconds - $lastStatsAt) -ge $StatsEveryMs) {
                Write-Host ("[{0}] idle: requests={1} slow={2} dropped={3}" -f (Stamp), $requests, $slow, $dropped)
                $lastStatsAt = $clock.ElapsedMilliseconds
                $intervalRequests = 0
            }
            continue
        }
        $client = $accept.Result
        $accept = $listener.AcceptTcpClientAsync()   # next client can queue while we answer this one

        try {
            $client.NoDelay = $true
            $client.ReceiveTimeout = $ReadTimeoutMs
            $client.SendTimeout = $ReadTimeoutMs
            $stream = $client.GetStream()
            $stream.ReadTimeout = $ReadTimeoutMs
            $stream.WriteTimeout = $ReadTimeoutMs

            $req = Read-HttpRequest $stream
            if ($null -eq $req) {
                $dropped++
            }
            else {
                $requests++
                $intervalRequests++
                if ($req.Method -eq 'POST' -and $req.Path -eq '/v1/position') {
                    $positions++
                    $isSlow = $false
                    if ($EveryMs -eq 0) {
                        $isSlow = $true
                    }
                    elseif (($clock.ElapsedMilliseconds - $lastSlowAt) -ge $EveryMs) {
                        $isSlow = $true
                        $lastSlowAt = $clock.ElapsedMilliseconds
                    }
                    if ($isSlow -and $DelayMs -gt 0) {
                        $slow++
                        Write-Host ("[{0}] slow answer #{1}: position reply held back {2} ms" -f (Stamp), $slow, $DelayMs)
                        Start-Sleep -Milliseconds $DelayMs
                    }
                    Send-Json $stream 200 $positionBody
                }
                elseif ($req.Method -eq 'GET' -and $req.Path -eq '/v1/status') {
                    $statuses++
                    Send-Json $stream 200 $statusBody
                }
                else {
                    $others++
                    Send-Json $stream 404 '{"ok":false,"error":"not found"}'
                }
            }
            try { $client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send) } catch { }
        }
        catch {
            $errors++   # the game cancelled the request / reset the socket: the next client is served normally
        }
        finally {
            $client.Close()
        }

        if (($clock.ElapsedMilliseconds - $lastStatsAt) -ge $StatsEveryMs) {
            Write-Host ("[{0}] requests={1} (position={2} status={3} other={4}) slow={5} dropped={6} errors={7}  last {8} s: {9} requests" -f
                (Stamp), $requests, $positions, $statuses, $others, $slow, $dropped, $errors,
                [int](($clock.ElapsedMilliseconds - $lastStatsAt) / 1000), $intervalRequests)
            $lastStatsAt = $clock.ElapsedMilliseconds
            $intervalRequests = 0
        }
    }
}
finally {
    $listener.Stop()
    Write-Host ("[{0}] stopped. requests={1} slow={2} dropped={3} errors={4}" -f (Stamp), $requests, $slow, $dropped, $errors)
}
