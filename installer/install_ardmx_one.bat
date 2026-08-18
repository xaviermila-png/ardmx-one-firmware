@echo off
setlocal enabledelayedexpansion
title Instal-lador de firmware ARDMX One

echo ============================================
echo   Instal-lador de firmware - ARDMX One
echo ============================================
echo.

set "PIO_PYTHON=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
set "ESPTOOL=%USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py"
set "BINDIR=%~dp0bin"

if not exist "%PIO_PYTHON%" (
    echo ERROR: no s'ha trobat el Python de PlatformIO a:
    echo   %PIO_PYTHON%
    echo Aquest instal-lador, de moment, nomes funciona en un ordinador
    echo amb PlatformIO instal-lat.
    goto :fi
)

if not exist "%ESPTOOL%" (
    echo ERROR: no s'ha trobat esptool.py a:
    echo   %ESPTOOL%
    goto :fi
)

if not exist "%BINDIR%\firmware.bin" (
    echo ERROR: falten els fitxers de firmware a la carpeta "bin".
    goto :fi
)

echo Ports serie disponibles:
echo.
set "AUTOPORT="
for /f "usebackq tokens=1,* delims=:" %%A in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0detect_port.ps1"`) do (
    if "%%A"=="LIST" echo   - %%B
    if "%%A"=="RECOMMENDED" set "AUTOPORT=%%B"
)
echo.

if defined AUTOPORT (
    echo Port recomanat, sembla l'ESP32: %AUTOPORT%
    echo.
    set /p COMPORT="Escriu el port COM, o prem Enter per fer servir %AUTOPORT%: "
    if "!COMPORT!"=="" set "COMPORT=!AUTOPORT!"
) else (
    set /p COMPORT="Escriu el port COM del ESP32, per exemple COM10, i prem Enter: "
)

if "%COMPORT%"=="" (
    echo No has escrit cap port. Sortint.
    goto :fi
)

echo.
echo Flashejant %COMPORT%... no desendollis el cable.
echo.

"%PIO_PYTHON%" "%ESPTOOL%" --chip esp32 --port %COMPORT% --baud 460800 ^
    --before default_reset --after hard_reset write_flash -z ^
    --flash_mode dio --flash_freq 40m --flash_size detect ^
    0x1000 "%BINDIR%\bootloader.bin" ^
    0x8000 "%BINDIR%\partitions.bin" ^
    0xe000 "%BINDIR%\boot_app0.bin" ^
    0x10000 "%BINDIR%\firmware.bin"

if errorlevel 1 (
    echo.
    echo ============================================
    echo   Hi ha hagut un error flashejant.
    echo   Comprova que el port es correcte i que
    echo   el cable USB esta be endollat.
    echo ============================================
) else (
    echo.
    echo ============================================
    echo   Firmware instal-lat correctament.
    echo ============================================
)

:fi
echo.
pause
