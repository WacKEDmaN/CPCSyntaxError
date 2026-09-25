@echo off
rem Configure and build CPCSyntaxError with MinGW-w64.
rem Needs the SDL2 MinGW development package unpacked into third_party\SDL2
rem (see third_party\SDL2\README.md), or pass -DSDL2_ROOT=... as an argument.
where ninja >nul 2>nul
if %errorlevel%==0 (set GEN=Ninja) else (set GEN=MinGW Makefiles)
cmake -S . -B build -G "%GEN%" -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo.
echo Built build\cpcse.exe
