@echo off
rem Compile l'exécutable d'un jeu (clang + ninja), cf. README.md.
rem Usage : build.bat <dossier du plugin du jeu> <nom de l'exécutable> [dossier de build] [dossier tiers]
rem Prérequis : <dossier de build>\gen généré (tools\recomp\gen.py + mk_entry.py), clang (LLVM) dans le PATH ;
rem vcvars64.bat : NX_VCVARS (par défaut, Visual Studio 18 Community).
setlocal
cd /d %~dp0
if "%~2"=="" (
  echo Usage : build.bat ^<plugin^> ^<exe^> [build] [tiers]
  exit /b 1
)
set JEU=%~f1
set EXE=%~2
set BUILD=%~f3
if "%~3"=="" set BUILD=%~dp0build
set TIERS=%~f4
if "%~4"=="" set TIERS=%~dp0tiers
if "%NX_VCVARS%"=="" set NX_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
call "%NX_VCVARS%" >nul || exit /b 1
cmake -S . -B "%BUILD%\cmake" -G Ninja -DCMAKE_C_COMPILER=clang.exe -DCMAKE_CXX_COMPILER=clang++.exe ^
      -DCMAKE_BUILD_TYPE=Release -DNX_JEU_DIR="%JEU%" -DNX_EXE=%EXE% -DNX_BUILD="%BUILD%" -DNX_TIERS="%TIERS%" || exit /b 1
cmake --build "%BUILD%\cmake" || exit /b 1
