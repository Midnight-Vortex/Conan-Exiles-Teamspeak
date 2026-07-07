@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /W4 /I ..\src hub_parser_test.c ..\src\core\hub\hub_parser.c /Fe:hub_parser_test.exe
if errorlevel 1 exit /b 1
hub_parser_test.exe
exit /b %errorlevel%
