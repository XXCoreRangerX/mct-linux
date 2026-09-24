@echo off
echo [%date% %time%] MCT T2 driver install start >> C:\t2install.log
set DRV=
for %%D in (D E F G H) do if exist %%D:\t2\TrgIdd64.inf set DRV=%%D:
if "%DRV%"=="" (
  echo DRIVER ISO NOT FOUND >> C:\t2install.log
  exit /b 1
)
echo Using driver source %DRV%\t2 >> C:\t2install.log
mkdir C:\t2files 2>nul
copy %DRV%\t2\* C:\t2files\ >> C:\t2install.log 2>&1
pnputil /add-driver %DRV%\t2\TrgIdd64.inf /install >> C:\t2install.log 2>&1
echo pnputil exit=%errorlevel% >> C:\t2install.log
echo [%date% %time%] done >> C:\t2install.log
