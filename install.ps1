# AboIDE One-Line Installer for Windows
Write-Host "🚀 Installing AboIDE..." -ForegroundColor Cyan
try { python --version | Out-Null } catch {
    Write-Host "❌ Python not found! Installing Python..." -ForegroundColor Red
    $pythonInstaller = "$env:TEMP\python_installer.exe"
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.11.0/python-3.11.0-amd64.exe" -OutFile $pythonInstaller
    Start-Process -FilePath $pythonInstaller -ArgumentList "/quiet InstallAllUsers=1 PrependPath=1" -Wait
    Remove-Item $pythonInstaller
    Write-Host "✅ Python installed!" -ForegroundColor Green
}
if (Test-Path "AboIDE") { Remove-Item -Recurse -Force "AboIDE" }
git clone https://github.com/ransombiyato/AboIDE.git
cd AboIDE
pip install pygame mutagen
python AboIDE.py
