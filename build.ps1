# Build Release x64, wrap a .ts3_plugin package, deploy DLL to %APPDATA%\TS3Client\plugins
# Usage:
#   .\build.ps1                  build + .ts3_plugin + deploy (does NOT start Package Installer)
#   .\build.ps1 -SkipDeploy       build + .ts3_plugin (no AppData copy)
#   .\build.ps1 -SkipPackage      build + deploy (no .ts3_plugin)
#   .\build.ps1 -SkipDeploy -OpenPackage
#                                 start TeamSpeak 3 Package Installer (TS must be quit)
#   .\build.ps1 -DeployOnly       deploy existing bin\conan_exiles.dll
#   .\build.ps1 -Pause            wait for keypress before exit

param(
    [switch]$SkipDeploy,
    [switch]$SkipPackage,
    [switch]$SkipInstaller,
    [switch]$OpenPackage,
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

    $src = Get-Item $SourceDll
    if ($src.Length -eq 0) {
        throw "Deploy aborted: source DLL is 0 bytes ($SourceDll)"
    }

    Copy-Item -Force $SourceDll $targetDll

    $deployed = Get-Item $targetDll
    if ($deployed.Length -eq 0) {
        throw "Deploy aborted: target DLL is 0 bytes (TeamSpeak still locking the file?)"
    }
    Log "  Deploy OK"
    Log "  Size: $($deployed.Length) bytes"
    Log "  Time: $($deployed.LastWriteTime)"
}

# .ts3_plugin = ZIP at archive root: package.ini + plugins/conan_exiles.dll
# Double-click / browser "Open with" launches TeamSpeak 3 Package Installer.
function New-Ts3PluginPackage {
    param(
        [string]$SourceDll,
        [switch]$OpenInstaller
    )

    $entryC = Join-Path $RootDir "src\ts\entry\ts3_entry.c"
    $template = Join-Path $RootDir "packaging\package.ini.in"
    if (-not (Test-Path $entryC)) {
        throw "Missing $entryC"
    }
    if (-not (Test-Path $template)) {
        throw "Missing $template"
    }
    if (-not (Test-Path $SourceDll)) {
        throw "DLL not found: $SourceDll"
    }

    $entryText = Get-Content -Raw -Path $entryC
    if ($entryText -notmatch 'ts3plugin_version\s*\(\s*void\s*\)\s*\{[^}]*return\s*"([^"]+)"') {
        throw "Could not parse ts3plugin_version() from ts3_entry.c"
    }
    $version = $Matches[1]

    $binDir = Split-Path $SourceDll -Parent
    $pkgName = "conan_exiles-$version-win64.ts3_plugin"
    $pkgPath = Join-Path $binDir $pkgName
    $zipPath = Join-Path $binDir ($pkgName + ".zip")

    $stage = Join-Path $env:TEMP ("ce-ts3-pkg-" + [guid]::NewGuid().ToString("N"))
    $pluginsDir = Join-Path $stage "plugins"
    New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
    Copy-Item -Force $SourceDll (Join-Path $pluginsDir "conan_exiles.dll")

    $ini = (Get-Content -Raw -Path $template) -replace '@VERSION@', $version
    $iniPath = Join-Path $stage "package.ini"
    [System.IO.File]::WriteAllText($iniPath, $ini.TrimEnd() + "`r`n")

    Remove-Item -Force -ErrorAction SilentlyContinue $pkgPath, $zipPath
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage,
        $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

    Move-Item -Force $zipPath $pkgPath
    Remove-Item -Recurse -Force $stage

    $pkg = Get-Item $pkgPath
    Log ""
    Log "Package OK: $($pkg.FullName)"
    Log "Size:       $($pkg.Length) bytes"
    Log "Doppelklick oeffnet TeamSpeak 3 Package Installer."

    if ($OpenInstaller) {
        Log "Starting TeamSpeak 3 Package Installer..."
        Start-Process -FilePath $pkgPath | Out-Null
    }

    return $pkgPath
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
            throw "Built DLL is missing the nickRand PROFILE marker - source/out of sync or stale tree"
        }
        Log "Verify: nickRand marker present in DLL"

        New-Item -ItemType Directory -Force -Path (Split-Path $BinDll) | Out-Null
        Copy-Item -Force $OutDll $BinDll

        $info = Get-Item $BinDll
        Log ""
        Log "Build OK: $($info.FullName)"
        Log "Size:     $([math]::Round($info.Length / 1MB, 2)) MB"
        Log "Time:     $($info.LastWriteTime)"

        if (-not $SkipPackage) {
            # Never auto-launch Package Installer during a deploy: it truncates
            # plugins\conan_exiles.dll to 0 bytes if TeamSpeak still holds the file.
            $launchInstaller = $OpenPackage -and -not $SkipInstaller -and $SkipDeploy
            if ($OpenPackage -and -not $SkipDeploy) {
                Log "OpenPackage ignored while deploying — installer would overwrite AppData. Use -SkipDeploy -OpenPackage, or double-click the .ts3_plugin after Quit."
            }
            New-Ts3PluginPackage -SourceDll $BinDll -OpenInstaller:$launchInstaller | Out-Null
        }
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
