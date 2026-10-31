@echo off
rem Builds build\opengl32.dll (x64) with the Visual Studio C++ tools.
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -products * -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%%i"
)
if not defined VSDIR (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\gen_exports.ps1" || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"
ml64 /nologo /c /Fothunk.obj "%ROOT%src\thunk.asm" || goto :fail
ml64 /nologo /c /Fostubs.obj "%ROOT%src\gen\stubs.asm" || goto :fail
cl /nologo /c /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%src" /I"%ROOT%third_party\imgui" /I"%ROOT%third_party\vulkan\include" "%ROOT%src\proxy.cpp" "%ROOT%src\hooks.cpp" "%ROOT%src\capture.cpp" "%ROOT%src\gfx.cpp" "%ROOT%src\overlay.cpp" "%ROOT%src\engine.cpp" "%ROOT%src\vulkan.cpp" "%ROOT%third_party\imgui\imgui.cpp" "%ROOT%third_party\imgui\imgui_draw.cpp" "%ROOT%third_party\imgui\imgui_tables.cpp" "%ROOT%third_party\imgui\imgui_widgets.cpp" "%ROOT%third_party\imgui\imgui_impl_opengl3.cpp" "%ROOT%third_party\imgui\imgui_impl_win32.cpp" || goto :fail
link /nologo /DLL /OUT:opengl32.dll /DEF:"%ROOT%src\gen\exports.def" thunk.obj stubs.obj proxy.obj hooks.obj capture.obj gfx.obj overlay.obj engine.obj vulkan.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_opengl3.obj imgui_impl_win32.obj user32.lib gdi32.lib ole32.lib windowscodecs.lib winmm.lib || goto :fail
popd
echo Built %ROOT%build\opengl32.dll
exit /b 0
:fail
popd
exit /b 1
