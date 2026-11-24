@echo off
setlocal
if "%~5"=="" (
  echo Run in an x64 Visual Studio developer prompt.
  echo Usage: build.cmd ReShadeInclude ImGuiInclude NgxInclude XeSSInclude MinHookRoot
  exit /b 2
)
set "RESH=%~f1"
set "IMGUI=%~f2"
set "NGX=%~f3"
set "XESS=%~f4"
set "MINHOOK=%~f5"
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /c /W3 /O2 /MD /I"%MINHOOK%\include" /Fo:build\ ^
  "%MINHOOK%\src\buffer.c" "%MINHOOK%\src\hook.c" "%MINHOOK%\src\trampoline.c" "%MINHOOK%\src\hde\hde64.c" || exit /b 1
cl /nologo /std:c++20 /W4 /EHsc /O2 /MD /LD /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /I"%RESH%" /I"%IMGUI%" /I"%NGX%" /I"%XESS%" /I"%MINHOOK%\include" ^
  /Fo:build\ fh4_dlss5_addon.cpp ngx_scene.cpp fh4_fg.cpp ^
  /link /OUT:build\fh4-dlss5.addon64 /IMPLIB:build\fh4-dlss5.lib d3d12.lib d3dcompiler.lib ^
  user32.lib build\buffer.obj build\hook.obj build\trampoline.obj build\hde64.obj || exit /b 1
cl /nologo /std:c++20 /W4 /EHsc /O2 /MD /LD /Fo:build\ nvngx-bridge.cpp ^
  /link /OUT:build\dlss5_nvngx.dll /IMPLIB:build\dlss5_nvngx.lib
exit /b %errorlevel%
