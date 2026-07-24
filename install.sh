#!/bin/bash
echo "🚀 Installing AboIDE..."
if ! command -v python3 &> /dev/null; then
    echo "❌ Python not found! Installing Python..."
    if [[ "$OSTYPE" == "darwin"* ]]; then
        brew install python3
    else
        sudo apt update
        sudo apt install -y python3 python3-pip
    fi
fi
rm -rf AboIDE
git clone https://github.com/ransombiyato/AboIDE.git
cd AboIDE
pip3 install pygame mutagen
python3 AboIDE.py
