@echo off
setlocal
cd /d "%~dp0"
if not exist build\Mods mkdir build\Mods
if not exist build\tests mkdir build\tests
pushd src
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /LD AlphaWater.cpp AlphaReeds.cpp /Fe..\build\Mods\AlphaWater.dll /link advapi32.lib user32.lib /EXPORT:WaterWorker=_WaterWorker@16
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE ReedShoreTest.cpp /Fe..\build\tests\ReedShoreTest.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE ReedOwnershipTest.cpp /Fe..\build\tests\ReedOwnershipTest.exe
set "result=%ERRORLEVEL%"
popd
exit /b %result%
