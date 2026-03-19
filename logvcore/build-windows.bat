@echo off
REM Build _logvcore pybind11 extension on Windows.
REM Run from a "Developer Command Prompt for VS" or after calling vcvarsall.bat.
REM Requires: Python 3.10+, pybind11 (pip install pybind11)
setlocal

cd /d "%~dp0"

for /f "delims=" %%i in ('python -c "import pybind11; print(pybind11.get_include())"') do set PB=%%i
for /f "delims=" %%i in ('python -c "import sysconfig; print(sysconfig.get_path(\"include\"))"') do set PYINC=%%i
for /f "delims=" %%i in ('python -c "import sysconfig; print(sysconfig.get_config_var(\"EXT_SUFFIX\"))"') do set EXT=%%i
for /f "delims=" %%i in ('python -c "import sysconfig, os; print(os.path.join(os.path.dirname(sysconfig.get_path(\"include\")), \"libs\"))"') do set PYLIBS=%%i

set OUT=..\\_logvcore%EXT%
echo pybind11 : %PB%
echo python   : %PYINC%
echo pylibs   : %PYLIBS%
echo output   : %OUT%

if not exist obj mkdir obj

REM --- Compile ---
cl /nologo /std:c++17 /O2 /EHsc /MD /Iinclude /c src\log_parser.cpp /Foobj\log_parser.obj
cl /nologo /std:c++17 /O2 /EHsc /MD /Iinclude /c src\log_filter.cpp /Foobj\log_filter.obj
cl /nologo /std:c++17 /O1 /EHsc /MD /Iinclude /I"%PB%" /I"%PYINC%" /c src\bindings.cpp /Foobj\bindings.obj

REM --- Link ---
link /nologo /DLL /OUT:%OUT% obj\log_parser.obj obj\log_filter.obj obj\bindings.obj /LIBPATH:"%PYLIBS%"

echo Built: %OUT%
python -c "import sys; sys.path.insert(0,'..'); import _logvcore; e=_logvcore.parse_line('Mar 17 12:00:00 host svc[1]: <6>[INFO](tid=1) test'); print('SMOKE TEST OK:', repr(e))"
