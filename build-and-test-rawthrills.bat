@echo off
setlocal EnableDelayedExpansion
:: Builds the Windows linuxloader with the existing MSYS2 (C:\msys64),
:: packages it into dist-win32\ with its DLLs and ll-deps, then runs it
:: against a Raw Thrills game and saves the logs.
::
:: Usage: build-and-test-rawthrills.bat [game folder or .squashfs]
::        (default: L:\CruisnBlast.squashfs)
:: A .squashfs image is extracted once next to the loader (the Windows
:: loader cannot mount it), into dist-win32\games\<name>.

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=L:\CruisnBlast.squashfs"
set "MSYS=C:\msys64"
set "OUT=%SRC%\dist-win32"

if not exist "%MSYS%\usr\bin\bash.exe" (
    echo MSYS2 not found at %MSYS%. Run scripts\windows\windows-build.bat once first.
    pause & exit /b 1
)

echo [1/4] Making sure the MinGW32 toolchain is installed...
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-ninja" >"%SRC%\toolchain.log" 2>&1

echo [2/4] Building (log: build-win32\build.log)...
set "CHERE_INVOKING=1"
set "MSYSTEM=MINGW32"
pushd "%SRC%"
:: The old cache was configured from a different path/generator; start clean.
if exist build-win32\CMakeCache.txt del /q build-win32\CMakeCache.txt
if not exist build-win32 md build-win32
"%MSYS%\usr\bin\bash.exe" -lc "cmake -S . -B build-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-win32 -- -k 0" >build-win32\build.log 2>&1
popd
if not exist "%SRC%\build-win32\linuxloader.exe" (
    echo BUILD FAILED - see build-win32\build.log
    pause & exit /b 1
)

echo [3/4] Packaging to dist-win32\ ...
if not exist "%OUT%\ll-deps" md "%OUT%\ll-deps"
copy /y "%SRC%\build-win32\linuxloader.exe" "%OUT%\" >nul
xcopy /y /e /i /q "%SRC%\libs\win32\ll-deps" "%OUT%\ll-deps" >nul
:: The game-side DLLs the loader opens from its own folder too.
for %%F in (cg.dll cg2.dll cgGL.dll openal32.dll zlib1.dll) do copy /y "%SRC%\libs\win32\ll-deps\%%F" "%OUT%\" >nul
for %%F in (libwinpthread-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll) do if exist "%MSYS%\mingw32\bin\%%F" copy /y "%MSYS%\mingw32\bin\%%F" "%OUT%\" >nul

if /i not "%GAME:~-9%"==".squashfs" goto run
for %%I in ("%GAME%") do set "GNAME=%%~nI"
set "GDIR=%OUT%\games\%GNAME%"
if exist "%GDIR%\" goto haveGame
echo Extracting "%GAME%" to "%GDIR%" (one time, may take several minutes)...
if not exist "%OUT%\games" md "%OUT%\games"
if not exist "%MSYS%\usr\bin\unsquashfs.exe" "%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm squashfs-tools" >"%OUT%\unsquashfs.log" 2>&1
if exist "%MSYS%\usr\bin\unsquashfs.exe" "%MSYS%\usr\bin\unsquashfs.exe" -no-xattrs -d "%GDIR%" "%GAME%" >>"%OUT%\unsquashfs.log" 2>&1
if exist "%GDIR%\" goto haveGame
if exist "%ProgramFiles%\7-Zip\7z.exe" "%ProgramFiles%\7-Zip\7z.exe" x -y -o"%GDIR%" "%GAME%" >>"%OUT%\unsquashfs.log" 2>&1
if exist "%GDIR%\" goto haveGame
echo Could not extract the image - see dist-win32\unsquashfs.log
pause
exit /b 1
:haveGame
set "GAME=%GDIR%"

:run
echo [4/4] Running the loader against "%GAME%" (log: dist-win32\test-run.log)...
pushd "%OUT%"
:: Warnings only by default: info logging writes thousands of lines a second
:: in a race and makes the game stutter. Set LL_LOG_LEVEL=info to debug.
if "%LL_LOG_LEVEL%"=="" set "LL_LOG_LEVEL=warn"
linuxloader.exe -g "%GAME%" >test-run.log 2>&1
set "RC=!ERRORLEVEL!"
popd
echo Loader exited with code %RC%. Log: %OUT%\test-run.log
pause
