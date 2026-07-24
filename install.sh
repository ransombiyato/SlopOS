#!/bin/bash
echo "🚀 Installing AboIDE..."

if command -v pkg &> /dev/null; then
    echo "📦 Detected Termux environment"
    if [ "$(whoami)" = "root" ]; then
        echo "❌ Error: You are running as root in Termux. 'pkg' does not work as root."
        echo "   Please run this script as a normal user, or switch to proot-distro."
        exit 1
    fi
    pkg update -y
    pkg install sdl2 sdl2-image sdl2-mixer sdl2-ttf sdl2-gfx -y
    pkg install python python-pip -y
    pip install pygame mutagen
elif command -v apt &> /dev/null; then
    echo "📦 Detected Debian/Ubuntu environment"
    sudo apt update
    sudo apt install python3-pygame python3-mutagen python3-tk git -y
else
    echo "❌ Unsupported package manager. Please install Python, pip, tkinter, pygame, and mutagen manually."
    exit 1
fi

echo "📦 Cloning AboIDE..."
rm -rf AboIDE
git clone https://github.com/ransombiyato/AboIDE.git
cd AboIDE

echo "🎮 Launching AboIDE..."
python3 AboIDE.py
