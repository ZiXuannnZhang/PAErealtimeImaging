@echo off
setlocal
set "QT_DIR=D:\Qt\Qt6.8.0"
set "QT_BIN=%QT_DIR%\6.8.0\mingw_64\bin"
set "MINGW_BIN=%QT_DIR%\Tools\mingw1310_64\bin"
set "NINJA_BIN=%QT_DIR%\Tools\Ninja"
set "CMAKE_EXE=%QT_DIR%\Tools\CMake_64\bin\cmake.exe"
set "CUDA_BLD=%~dp0build\ring_recon_cuda"
set "CUDA_MIGRATION=%~dp0..\..\_migration_pack\prebuilt_cuda"
set "CUDA_IMPORT=%CUDA_BLD%\libring_recon_cuda.dll.a"
set "CUDA_BIN=%CUDA_BLD%\bin"
set "IMAGING_RUNTIME_DIR=%~dp0libs\imaging"
if defined PAIMAGE_IMAGING_RUNTIME_DIR set "IMAGING_RUNTIME_DIR=%PAIMAGE_IMAGING_RUNTIME_DIR%"
if not exist "%CUDA_IMPORT%" (
    set "CUDA_IMPORT=%CUDA_MIGRATION%\bin\libring_recon_cuda.dll.a"
    set "CUDA_BIN=%CUDA_MIGRATION%\bin"
)
if not exist "%CUDA_IMPORT%" (
    echo Missing ring CUDA import library: %CUDA_IMPORT%
    exit /b 2
)
if not exist "%CUDA_BIN%\ring_recon_cuda.dll" (
    echo Missing ring CUDA runtime directory: %CUDA_BIN%
    exit /b 2
)
if not exist "%IMAGING_RUNTIME_DIR%\pa_recon_core.dll" (
    echo Missing linear imaging runtime: %IMAGING_RUNTIME_DIR%\pa_recon_core.dll
    echo Set PAIMAGE_IMAGING_RUNTIME_DIR to an external imaging runtime bundle if needed.
    exit /b 2
)
if not exist "%IMAGING_RUNTIME_DIR%\cufft64_12.dll" (
    echo Missing linear imaging runtime: %IMAGING_RUNTIME_DIR%\cufft64_12.dll
    echo Set PAIMAGE_IMAGING_RUNTIME_DIR to an external imaging runtime bundle if needed.
    exit /b 2
)
set "PATH=%QT_BIN%;%MINGW_BIN%;%NINJA_BIN%;%PATH%"

if /I "%~1"=="configure" goto :configure
if /I "%~1"=="build" goto :build

"%CMAKE_EXE%" --preset mingw-debug -DRING_RECON_CUDA_IMPORT_LIB=%CUDA_IMPORT% -DRING_RECON_CUDA_BIN=%CUDA_BIN% "-DIMAGING_RUNTIME_DIR=%IMAGING_RUNTIME_DIR%"
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build --preset mingw-debug-build
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\mingw_debug\bin\PAimageReceiverDiagnostics.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ImagingSvc.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ring_svc_selftest.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ring_udp_replay.exe" exit /b 3
exit /b %errorlevel%

:configure
"%CMAKE_EXE%" --preset mingw-debug -DRING_RECON_CUDA_IMPORT_LIB=%CUDA_IMPORT% -DRING_RECON_CUDA_BIN=%CUDA_BIN% "-DIMAGING_RUNTIME_DIR=%IMAGING_RUNTIME_DIR%"
exit /b %errorlevel%

:build
"%CMAKE_EXE%" --build --preset mingw-debug-build
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\mingw_debug\bin\PAimageReceiverDiagnostics.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ImagingSvc.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ring_svc_selftest.exe" exit /b 3
if not exist "%~dp0build\mingw_debug\bin\ring_udp_replay.exe" exit /b 3
exit /b %errorlevel%
