param(
    [switch]$SkipDeploy,
    [switch]$DeployOnly
)

$ErrorActionPreference = "Stop"

function Log([string]$Message) {
    Write-Output ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Message)
}

function Deploy-ToTeamSpeak {
    param(
        [string]$SourceDll,
        [int]$WaitSeconds = 180
    )

    $pluginsDir = Join-Path $env:APPDATA "TS3Client\plugins"
    $targetDll = Join-Path $pluginsDir "conan_exiles.dll"

    New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null

    Log ""
    Log "Deploying to TeamSpeak plugins folder..."
    Log "  Source: $SourceDll"
    Log "  Target: $targetDll"

    if (Get-Process ts3client_win64 -ErrorAction SilentlyContinue) {
        Log "  TeamSpeak laeuft noch - warte auf Beenden (max ${WaitSeconds}s)..."
        $deadline = (Get-Date).AddSeconds($WaitSeconds)
        while ((Get-Date) -lt $deadline) {
            if (-not (Get-Process ts3client_win64 -ErrorAction SilentlyContinue)) {
                Log "  TeamSpeak beendet."
                break
            }
            Start-Sleep -Seconds 2
        }
    }

    if (Get-Process ts3client_win64 -ErrorAction SilentlyContinue) {
        throw @"
Deploy fehlgeschlagen: TeamSpeak blockiert die DLL.
Bitte TeamSpeak komplett beenden (Tray -> Quit) und erneut ausfuehren:
  powershell -File build\build_msvc.ps1
"@
    }

    Copy-Item -Force $SourceDll $targetDll

    $deployed = Get-Item $targetDll
    Log "  Deploy OK"
    Log "  Size: $($deployed.Length) bytes"
    Log "  Time: $($deployed.LastWriteTime)"
}

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BinDll = Join-Path $RootDir "bin\conan_exiles.dll"

if (-not $DeployOnly) {
    $SdkHeader = Join-Path $RootDir "sdk\include\ts3_functions.h"
    if (-not (Test-Path $SdkHeader)) {
        Log ""
        Log "TeamSpeak SDK missing - cloning ts3client-pluginsdk..."
        git clone --depth 1 https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git (Join-Path $RootDir "sdk")
        if (-not (Test-Path $SdkHeader)) {
            throw "SDK clone failed - ts3_functions.h still missing"
        }
        Log "SDK OK"
    }

    $Vcxproj = Join-Path $RootDir "project\Conan-Exiles-TeamSpeak.vcxproj"

    $VsCandidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\BuildTools",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise"
    )

    $VsInstall = $null
    foreach ($path in $VsCandidates) {
        if (Test-Path $path) {
            $VsInstall = $path
            break
        }
    }

    $MsBuild = $null
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $MsBuild = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($MsBuild -and -not $VsInstall) {
            $VsInstall = Split-Path (Split-Path (Split-Path $MsBuild -Parent) -Parent) -Parent
        }
    }

    if (-not $VsInstall) {
        throw "Visual Studio not found. Install VS 2022/2026 with C++ desktop workload."
    }

    if (-not $MsBuild) {
        $MsBuild = Join-Path $VsInstall "MSBuild\Current\Bin\MSBuild.exe"
    }
    if (-not (Test-Path $MsBuild)) {
        throw "MSBuild not found: $MsBuild"
    }

    $MsvcRoot = Join-Path $VsInstall "VC\Tools\MSVC"
    if (-not (Test-Path $MsvcRoot)) {
        throw @"
C++ build tools missing in: $VsInstall
Open Visual Studio Installer -> Modify -> Desktopentwicklung mit C++
"@
    }

    $MsvcVer = (Get-ChildItem $MsvcRoot -Directory | Select-Object -First 1).Name
    Log "Conan Exiles TeamSpeak (Rewrite) - MSVC Build"
    Log "================================================"
    Log ""
    Log "Root:          $RootDir"
    Log "Visual Studio: $VsInstall"
    Log "MSBuild:       $MsBuild"
    Log "MSVC:          $MsvcVer (toolset v145)"
    Log ""
    Log "Compiling Release x64..."
    Log ""

    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $MsBuild $Vcxproj /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal /nologo
    $exitCode = $LASTEXITCODE
    Log ""
    Log "MSBuild done in $([math]::Round($sw.Elapsed.TotalSeconds, 1))s (exit $exitCode)"

    if ($exitCode -ne 0) {
        throw "Build failed - see errors above"
    }

    $OutDll = Join-Path $RootDir "bin\x64\Release\conan_exiles.dll"
    if (-not (Test-Path $OutDll)) {
        throw "Output missing: $OutDll"
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $BinDll) | Out-Null
    Copy-Item -Force $OutDll $BinDll

    $info = Get-Item $BinDll
    Log ""
    Log "Build OK: $($info.FullName)"
    Log "Size:     $([math]::Round($info.Length / 1MB, 2)) MB"
    Log "Time:     $($info.LastWriteTime)"
}
else {
    Log "Deploy only (skip build)"
    if (-not (Test-Path $BinDll)) {
        throw "DLL not found: $BinDll - run build\build_msvc.ps1 without -DeployOnly first"
    }
}

if (-not $SkipDeploy) {
    Deploy-ToTeamSpeak -SourceDll $BinDll
    Log ""
    Log "Fertig. TeamSpeak neu starten und testen."
}
