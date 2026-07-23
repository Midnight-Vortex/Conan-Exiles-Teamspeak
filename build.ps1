# Build Release x64 and deploy conan_exiles.dll to %APPDATA%\TS3Client\plugins
# Usage:
#   .\build.ps1              build + deploy
#   .\build.ps1 -SkipDeploy   build only
#   .\build.ps1 -DeployOnly   deploy existing bin\conan_exiles.dll
#   .\build.ps1 -Pause        wait for keypress before exit

param(
    [switch]$SkipDeploy,
    [switch]$DeployOnly,
    [switch]$Pause
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
  .\build.ps1
"@
    }

    Copy-Item -Force $SourceDll $targetDll

    $deployed = Get-Item $targetDll
    Log "  Deploy OK"
    Log "  Size: $($deployed.Length) bytes"
    Log "  Time: $($deployed.LastWriteTime)"
}

$RootDir = $PSScriptRoot
$BinDll = Join-Path $RootDir "bin\conan_exiles.dll"
$exitCode = 0

try {
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
        & $MsBuild $Vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal /nologo
        $msBuildExit = $LASTEXITCODE
        Log ""
        Log "MSBuild done in $([math]::Round($sw.Elapsed.TotalSeconds, 1))s (exit $msBuildExit)"

        if ($msBuildExit -ne 0) {
            throw "Build failed - see errors above"
        }

        $OutDll = Join-Path $RootDir "bin\x64\Release\conan_exiles.dll"
        if (-not (Test-Path $OutDll)) {
            throw "Output missing: $OutDll"
        }

        $dllText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($OutDll))
        if ($dllText -notmatch 'nickRand') {
            throw "Built DLL is missing the nickRand PROFILE marker — source/out of sync or stale tree"
        }
        Log "Verify: nickRand marker present in DLL"

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
            throw "DLL not found: $BinDll - run .\build.ps1 without -DeployOnly first"
        }
    }

    if (-not $SkipDeploy) {
        Deploy-ToTeamSpeak -SourceDll $BinDll
        Log ""
        Log "Fertig. TeamSpeak neu starten und testen."
    }
}
catch {
    Log ""
    Log "Build/Deploy fehlgeschlagen: $($_.Exception.Message)"
    $exitCode = 1
}
finally {
    if ($Pause -or ($exitCode -ne 0 -and [Environment]::UserInteractive)) {
        Log ""
        Read-Host "Enter druecken zum Beenden"
    }
}

exit $exitCode
