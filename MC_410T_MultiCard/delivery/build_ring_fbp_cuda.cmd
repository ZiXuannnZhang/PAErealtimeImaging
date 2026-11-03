@echo off
setlocal
rem CUDA is built with MSVC; the application remains the standard MinGW Debug build.
rem Override these task-specific paths for a different installation.
if not defined FBP_VCVARS set "FBP_VCVARS=C:\Program\VC\Auxiliary\Build\vcvars64.bat"
if not defined FBP_QT_ROOT set "FBP_QT_ROOT=D:\Qt\Qt6.8.0"
if not defined FBP_CUDA_ROOT set "FBP_CUDA_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
if not exist "%FBP_VCVARS%" exit /b 2
call "%FBP_VCVARS%"
if errorlevel 1 exit /b %errorlevel%
set "PATH=%FBP_QT_ROOT%\Tools\Ninja;%FBP_CUDA_ROOT%\bin;%PATH%"
set "FBP_BUILD=%~dp0build\ring_recon_cuda"
"%FBP_QT_ROOT%\Tools\CMake_64\bin\cmake.exe" -S "%~dp0src\RingReconCuda" -B "%FBP_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCUDAToolkit_ROOT="%FBP_CUDA_ROOT%" -DCUDA_RUNTIME_DIR="%FBP_CUDA_ROOT%\bin"
if errorlevel 1 exit /b %errorlevel%
"%FBP_QT_ROOT%\Tools\CMake_64\bin\cmake.exe" --build "%FBP_BUILD%"
if errorlevel 1 exit /b %errorlevel%
pushd "%FBP_BUILD%"
"%FBP_QT_ROOT%\Tools\mingw1310_64\bin\gendef.exe" bin\ring_recon_cuda.dll
if errorlevel 1 (popd & exit /b 3)
"%FBP_QT_ROOT%\Tools\mingw1310_64\bin\dlltool.exe" -d ring_recon_cuda.def -l libring_recon_cuda.dll.a -D ring_recon_cuda.dll
if errorlevel 1 (popd & exit /b 3)
popd
exit /b 0
