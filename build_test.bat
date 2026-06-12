@echo off
echo === Monolith Build Test ===
cd /d "%~dp0"

"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" build\monolith.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal 2>&1

echo.
echo === Build finished with exit code %ERRORLEVEL% ===
pause
