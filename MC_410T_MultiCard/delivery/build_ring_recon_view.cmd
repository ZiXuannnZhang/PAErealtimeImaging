@echo off
setlocal
set "QT_DIR=D:\Qt\Qt6.8.0"
set "MINGW_BIN=%QT_DIR%\Tools\mingw1310_64\bin"
set "NINJA_BIN=%QT_DIR%\Tools\Ninja"
set "CMAKE_EXE=%QT_DIR%\Tools\CMake_64\bin\cmake.exe"
set "PATH=%MINGW_BIN%;%NINJA_BIN%;%PATH%"
set "SRC=%~dp0src\RingRecon"
set "BLD=%~dp0build\ring_recon_release"
set "CUDA_BLD=%~dp0build\ring_recon_cuda"

"%CMAKE_EXE%" -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER=%QT_DIR%/Tools/mingw1310_64/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=%NINJA_BIN%/ninja.exe ^
  -DQt6_DIR=%QT_DIR%/6.8.0/mingw_64/lib/cmake/Qt6 ^
  -DBUILD_RINGRECON_VIEW=ON ^
  -DRING_RECON_CUDA_IMPORT_LIB=%CUDA_BLD%\libring_recon_cuda.dll.a ^
  -DRING_RECON_CUDA_BIN=%CUDA_BLD%\bin
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build "%BLD%"
exit /b %errorlevel%