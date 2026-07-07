# Build Release x64 and deploy conan_exiles.dll to %APPDATA%\TS3Client\plugins
# Usage:
#   .\build-deploy.ps1              build + deploy
#   .\build-deploy.ps1 -SkipDeploy  build only
#   .\build-deploy.ps1 -DeployOnly  deploy existing bin\conan_exiles.dll

& "$PSScriptRoot\build\build_msvc.ps1" @args
exit $LASTEXITCODE
