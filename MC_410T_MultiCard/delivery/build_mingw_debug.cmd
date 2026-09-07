@echo off
setlocal
set "QT_DIR=D:\Qt\Qt6.8.0"
set "QT_BIN=%QT_DIR%\6.8.0\mingw_64\bin"
set "MINGW_BIN=%QT_DIR%\Tools\mingw1310_64\bin"
set "NINJA_BIN=%QT_DIR%\Tools\Ninja"
set "CMAKE_EXE=%QT_DIR%\Tools\CMake_64\bin\cmake.exe"
set "CUDA_BLD=%~dp0build\ring_recon_cuda"
set "PATH=%QT_BIN%;%MINGW_BIN%;%NINJA_BIN%;%PATH%"

if /I "%~1"=="configure" goto :configure
if /I "%~1"=="build" goto :build

"%CMAKE_EXE%" --preset mingw-debug -DRING_RECON_CUDA_IMPORT_LIB=%CUDA_BLD%\libring_recon_cuda.dll.a -DRING_RECON_CUDA_BIN=%CUDA_BLD%\bin
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build --preset mingw-debug-build
exit /b %errorlevel%

:configure
"%CMAKE_EXE%" --preset mingw-debug -DRING_RECON_CUDA_IMPORT_LIB=%CUDA_BLD%\libring_recon_cuda.dll.a -DRING_RECON_CUDA_BIN=%CUDA_BLD%\bin
exit /b %errorlevel%

:build
"%CMAKE_EXE%" --build --preset mingw-debug-build
exit /b %errorlevel%
