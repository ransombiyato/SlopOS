"""
MINI IDE - Complete Implementation
==================================
A tkinter-based IDE featuring:
  1. Code editor (with line numbers)
  2. Terminal console (real shell commands)
  3. 2D code runner / mini game engine viewer
  4. Save/export, Load/import files
  5. Multi-language syntax highlighting
  6. Code warnings (linter with hover tooltips)
  7. tlkr chatspace with public/private servers + join codes
  8. Settings persistence (theme, toggles, font size, autosave)
  9. Animated themes (gloomy, sunset, basement)
  10. Toggle switches for syntax highlighting / warnings
  11. Plugin system with Marketplace (local)
  12. CLI support (open file from command line)
  13. Music player with popup controls
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
import subprocess
import threading
import queue
import socket
import struct
import json
import os
import sys
import tokenize
import io
import ast
import re
import random
import time
import secrets
import string
import shlex
import importlib.util
import inspect
import shutil
import pygame
from mutagen import File as MutagenFile
import threading
import tempfile

# ======================================================================
#  CONFIGURATION / PERSISTENCE
# ======================================================================

CONFIG_DIR = os.path.expanduser("~/.mini_ide")
CONFIG_FILE = os.path.join(CONFIG_DIR, "config.json")
PLUGINS_DIR = os.path.join(CONFIG_DIR, "plugins")
MARKETPLACE_FILE = os.path.join(CONFIG_DIR, "marketplace.json")

DEFAULT_SETTINGS = {
    "syntax_highlight": True,
    "warnings": True,
    "chat_font_highlight": True,
    "autosave": True,
    "autosave_interval": 30,
    "font_size": 12,
    "theme": "Dark Classic",
    "plugins_enabled": True,
    "music_enabled": False,
    "music_repeat": False,
    "music_volume": 50,
}


def load_config():
    settings = DEFAULT_SETTINGS.copy()
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                saved = json.load(f)
                settings.update(saved)
        except (OSError, json.JSONDecodeError):
            pass
    return settings


def save_config(settings, theme_name):
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        data = {**settings, "theme": theme_name}
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except OSError:
        pass


def generate_join_code(length=8):
    alphabet = string.ascii_uppercase + string.digits
    return ''.join(secrets.choice(alphabet) for _ in range(length))


def ensure_directories():
    os.makedirs(CONFIG_DIR, exist_ok=True)
    os.makedirs(PLUGINS_DIR, exist_ok=True)
    if not os.path.exists(MARKETPLACE_FILE):
        with open(MARKETPLACE_FILE, "w") as f:
            json.dump([], f)


ensure_directories()

# Initialize pygame mixer for music
pygame.mixer.init()

# ======================================================================
#  THEMES
# ======================================================================

THEMES = {
    "Dark Classic": {
        "bg": "#1e1e1e", "fg": "#d4d4d4", "editor_bg": "#1e1e1e",
        "console_bg": "#0c0c0c", "console_fg": "#00ff5f",
        "accent": "#007acc", "warning": "#ff5f5f",
        "keyword": "#569cd6", "string": "#ce9178", "comment": "#6a9955",
        "number": "#b5cea8", "func": "#dcdcaa", "animated": None,
    },
    "Light Classic": {
        "bg": "#ffffff", "fg": "#1e1e1e", "editor_bg": "#fafafa",
        "console_bg": "#f0f0f0", "console_fg": "#005500",
        "accent": "#0066cc", "warning": "#cc0000",
        "keyword": "#0000ff", "string": "#a31515", "comment": "#008000",
        "number": "#098658", "func": "#795e26", "animated": None,
    },
    "Sunset": {
        "bg": "#2d1b2e", "fg": "#ffe0c2", "editor_bg": "#3a2035",
        "console_bg": "#1a0f1a", "console_fg": "#ffb347",
        "accent": "#ff6f61", "warning": "#ff3860",
        "keyword": "#ff9e64", "string": "#ffd27f", "comment": "#c97064",
        "number": "#ffcf70", "func": "#ff7ab3", "animated": "sunset",
    },
    "Gloomy": {
        "bg": "#0f1620", "fg": "#c9d6e3", "editor_bg": "#111a24",
        "console_bg": "#0a1017", "console_fg": "#7fdbff",
        "accent": "#39a0ed", "warning": "#ff6b6b",
        "keyword": "#4fa3e3", "string": "#9adcff", "comment": "#557a95",
        "number": "#b0e0e6", "func": "#8ecae6", "animated": "gloomy",
    },
    "Basement": {
        "bg": "#0a0a0a", "fg": "#a0a0a0", "editor_bg": "#0d0d0d",
        "console_bg": "#050505", "console_fg": "#e0e0e0",
        "accent": "#555555", "warning": "#ff4444",
        "keyword": "#888888", "string": "#666666", "comment": "#444444",
        "number": "#999999", "func": "#777777", "animated": "basement",
    },
    "Mineral": {
        "bg": "#101820", "fg": "#e8f4f8", "editor_bg": "#0d1b1e",
        "console_bg": "#081012", "console_fg": "#50c878",
        "accent": "#0f52ba", "warning": "#e0115f",
        "keyword": "#0f52ba", "string": "#e0115f", "comment": "#50c878",
        "number": "#ffc87c", "func": "#b9f2ff", "animated": None,
    },
}

# ======================================================================
#  TOOLTIP
# ======================================================================

class ToolTip:
    def __init__(self, widget):
        self.widget = widget
        self.tip = None

    def show(self, text, x, y):
        self.hide()
        self.tip = tk.Toplevel(self.widget)
        self.tip.wm_overrideredirect(True)
        self.tip.wm_geometry(f"+{x+15}+{y+10}")
        label = tk.Label(
            self.tip, text=text, background="#ffffe0", foreground="#000000",
            relief="solid", borderwidth=1, font=("Consolas", 9), justify="left",
            wraplength=350,
        )
        label.pack()

    def hide(self):
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None

# ======================================================================
#  SYNTAX HIGHLIGHTER (multi‑language)
# ======================================================================

class SyntaxHighlighter:
    LANGUAGE_PATTERNS = {
        "python": {
            "keywords": r"\b(False|None|True|and|as|assert|async|await|break|class|continue|"
                        r"def|del|elif|else|except|finally|for|from|global|if|import|in|is|"
                        r"lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield)\b",
            "string": r'(\"\"\".*?\"\"\"|\'\'\'.*?\'\'\'|\".*?\"|\'.*?\')',
            "comment": r'#.*',
            "number": r'\b\d+(\.\d+)?\b',
            "function": r'\bdef\s+(\w+)',
        },
        "javascript": {
            "keywords": r"\b(break|case|catch|class|const|continue|debugger|default|delete|"
                        r"do|else|export|extends|finally|for|function|if|import|in|instanceof|"
                        r"new|return|super|switch|this|throw|try|typeof|var|void|while|with|"
                        r"yield|let|static|await|async)\b",
            "string": r'(\"\"\".*?\"\"\"|\'\'\'.*?\'\'\'|\".*?\"|\'.*?\')',
            "comment": r'//.*|/\*.*?\*/',
            "number": r'\b\d+(\.\d+)?\b',
            "function": r'\bfunction\s+(\w+)|(\w+)\s*\(',
        },
        "html": {
            "keywords": r'</?[\w-]+[^>]*>',
            "string": r'"[^"]*"|\'[^\']*\'',
            "comment": r'<!--.*?-->',
            "number": r'\b\d+\b',
            "function": None,
        },
        "css": {
            "keywords": r'[.#][\w-]+|[\w-]+\s*\{',
            "string": r'"[^"]*"|\'[^\']*\'',
            "comment": r'/\*.*?\*/',
            "number": r'\b\d+(\.\d+)?(px|em|%|rem|vw|vh)?\b',
            "function": None,
        },
        "json": {
            "keywords": r'"[^"]+":',
            "string": r'"[^"]*"',
            "comment": None,
            "number": r'\b\d+(\.\d+)?\b',
            "function": None,
        },
    }

    def __init__(self, text_widget, theme):
        self.text = text_widget
        self.set_theme(theme)
        self.language = "python"
        self._configure_tags()

    def set_language(self, language):
        if language in self.LANGUAGE_PATTERNS:
            self.language = language

    def detect_language_from_filename(self, filename):
        ext = os.path.splitext(filename)[1].lower() if filename else ""
        ext_map = {
            ".py": "python",
            ".js": "javascript",
            ".html": "html", ".htm": "html",
            ".css": "css",
            ".json": "json",
        }
        self.language = ext_map.get(ext, "python")
        return self.language

    def set_theme(self, theme):
        self.theme = theme

    def _configure_tags(self):
        t = self.theme
        self.text.tag_configure("kw", foreground=t["keyword"])
        self.text.tag_configure("str", foreground=t["string"])
        self.text.tag_configure("cmt", foreground=t["comment"])
        self.text.tag_configure("num", foreground=t["number"])
        self.text.tag_configure("func", foreground=t["func"])

    def highlight(self, enabled=True):
        for tag in ("kw", "str", "cmt", "num", "func"):
            self.text.tag_remove(tag, "1.0", "end")
        if not enabled:
            return

        content = self.text.get("1.0", "end-1c")
        patterns = self.LANGUAGE_PATTERNS.get(self.language, self.LANGUAGE_PATTERNS["python"])

        def apply_pattern(pattern, tag, flags=0):
            if pattern is None:
                return
            for m in re.finditer(pattern, content, flags):
                start = f"1.0+{m.start()}c"
                end = f"1.0+{m.end()}c"
                self.text.tag_add(tag, start, end)

        if patterns.get("comment"):
            apply_pattern(patterns["comment"], "cmt", re.DOTALL)
        if patterns.get("string"):
            apply_pattern(patterns["string"], "str", re.DOTALL)
        if patterns.get("number"):
            apply_pattern(patterns["number"], "num")
        if patterns.get("function"):
            apply_pattern(patterns["function"], "func")
        if patterns.get("keywords"):
            apply_pattern(patterns["keywords"], "kw")

# ======================================================================
#  LINTER
# ======================================================================

class Linter:
    @staticmethod
    def check(code: str):
        warnings = {}

        try:
            ast.parse(code)
        except SyntaxError as e:
            if e.lineno:
                warnings.setdefault(e.lineno, []).append(f"SyntaxError: {e.msg}")

        for i, line in enumerate(code.splitlines(), start=1):
            if line.strip() == "":
                continue
            leading = len(line) - len(line.lstrip(" "))
            if "\t" in line[:leading]:
                warnings.setdefault(i, []).append("Tabs used for indentation (use spaces).")
            elif leading % 4 != 0:
                warnings.setdefault(i, []).append(
                    f"Indentation is {leading} spaces (expected a multiple of 4)."
                )

        try:
            list(tokenize.generate_tokens(io.StringIO(code).readline))
        except tokenize.TokenizeError:
            pass
        except IndentationError as e:
            if e.lineno:
                warnings.setdefault(e.lineno, []).append(f"IndentationError: {e.msg}")

        for i, line in enumerate(code.splitlines(), start=1):
            stripped = line.rstrip("\n")
            if stripped != stripped.rstrip():
                warnings.setdefault(i, []).append("Trailing whitespace.")
            if len(stripped) > 99:
                warnings.setdefault(i, []).append(f"Line too long ({len(stripped)} > 99 characters).")
            if re.search(r"\bexcept\s*:", stripped):
                warnings.setdefault(i, []).append("Bare 'except:' - prefer 'except Exception:'.")
            if re.search(r"==\s*None\b|!=\s*None\b", stripped):
                warnings.setdefault(i, []).append("Use 'is None' / 'is not None'.")
            if re.search(r"def\s+\w+\([^)]*=\s*(\[\]|\{\}|\(\))", stripped):
                warnings.setdefault(i, []).append("Mutable default argument - use None.")
            if re.search(r"\blambda\b.*:\s*$", stripped):
                warnings.setdefault(i, []).append("Lambda body looks empty.")

        try:
            tree = ast.parse(code)
            for node in ast.walk(tree):
                body = getattr(node, "body", None)
                if isinstance(body, list):
                    for idx, stmt in enumerate(body[:-1]):
                        if isinstance(stmt, (ast.Return, ast.Raise, ast.Break, ast.Continue)):
                            nxt = body[idx + 1]
                            if hasattr(nxt, 'lineno'):
                                warnings.setdefault(nxt.lineno, []).append("Unreachable code.")

            imported_names = set()
            used_names = set()
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    for alias in node.names:
                        imported_names.add(alias.name.split('.')[0])
                elif isinstance(node, ast.ImportFrom):
                    if node.module:
                        imported_names.add(node.module.split('.')[0])
                elif isinstance(node, ast.Name):
                    if isinstance(node.ctx, ast.Load):
                        used_names.add(node.id)

            for name in imported_names:
                if name not in used_names and name not in {"sys", "os", "tkinter", "re", "math", "random"}:
                    for stmt in tree.body:
                        if isinstance(stmt, (ast.Import, ast.ImportFrom)):
                            for alias in getattr(stmt, 'names', []):
                                if alias.name.split('.')[0] == name:
                                    warnings.setdefault(stmt.lineno, []).append(f"Unused import: '{name}'")
                                    break

            for node in ast.walk(tree):
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    complexity = 1
                    for child in ast.walk(node):
                        if isinstance(child, (ast.If, ast.While, ast.For, ast.And, ast.Or,
                                              ast.Try, ast.ExceptHandler, ast.With,
                                              ast.Lambda, ast.AsyncFunctionDef,
                                              ast.ClassDef)):
                            complexity += 1
                    if complexity > 10:
                        warnings.setdefault(node.lineno, []).append(
                            f"High cyclomatic complexity: {complexity} (threshold: 10)"
                        )
        except SyntaxError:
            pass

        combined = {}
        for line, msgs in warnings.items():
            combined[line] = "\n".join(msgs)
        return combined

# ======================================================================
#  LINE NUMBERS
# ======================================================================

class LineNumbers(tk.Canvas):
    def __init__(self, master, text_widget, **kwargs):
        super().__init__(master, width=42, **kwargs)
        self.text_widget = text_widget

    def redraw(self):
        self.delete("all")
        i = self.text_widget.index("@0,0")
        while True:
            dline = self.text_widget.dlineinfo(i)
            if dline is None:
                break
            y = dline[1]
            linenum = str(i).split(".")[0]
            self.create_text(36, y, anchor="ne", text=linenum,
                             fill=self["fg"] if "fg" in self.keys() else "#888")
            i = self.text_widget.index(f"{i}+1line")

# ======================================================================
#  CODE EDITOR FRAME
# ======================================================================

class CodeEditorFrame(tk.Frame):
    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self.current_path = None

        self.text = tk.Text(self, wrap="none", undo=True, font=("Consolas", 12),
                            insertbackground="white", borderwidth=0)
        self.linenumbers = LineNumbers(self, self.text, highlightthickness=0)

        yscroll = ttk.Scrollbar(self, orient="vertical", command=self.text.yview)
        xscroll = ttk.Scrollbar(self, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)

        self.linenumbers.grid(row=0, column=0, sticky="ns")
        self.text.grid(row=0, column=1, sticky="nsew")
        yscroll.grid(row=0, column=2, sticky="ns")
        xscroll.grid(row=1, column=1, sticky="ew")
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(1, weight=1)

        self.highlighter = SyntaxHighlighter(self.text, app.theme)
        self.tooltip = ToolTip(self.text)
        self.warnings = {}
        self._dirty = False
        self._autosave_job = None

        self.text.bind("<KeyRelease>", self._on_change)
        self.text.bind("<MouseWheel>", lambda e: self.linenumbers.redraw())
        self.text.bind("<Configure>", lambda e: self.linenumbers.redraw())
        self.text.tag_bind("warn", "<Enter>", self._show_warning)
        self.text.tag_bind("warn", "<Leave>", lambda e: self.tooltip.hide())

        self.text.insert(
            "1.0",
            "# Welcome to Mini IDE\n"
            "# Write Python here. Try the 2D Runner tab, or Run in the Console.\n\n"
            "def setup(engine):\n"
            "    engine.rect('player', 50, 50, 40, 40, color='#39a0ed')\n"
            "    engine.rect('ground', 0, 300, 480, 20, color='#4a6a7f')\n"
            "    engine.circle('coin', 400, 100, 15, color='#ffd700')\n\n"
            "def update(engine, dt):\n"
            "    if engine.is_key('Left'):\n"
            "        engine.move('player', dx=-150*dt, dy=0)\n"
            "    if engine.is_key('Right'):\n"
            "        engine.move('player', dx=150*dt, dy=0)\n"
            "    if engine.is_key_pressed('space') and engine.is_on_ground('player'):\n"
            "        engine.set_vel('player', vy=-300)\n"
        )
        self._on_change()
        self._schedule_autosave()

    def _on_change(self, event=None):
        self._dirty = True
        self.linenumbers.redraw()
        if self.app.settings["syntax_highlight"]:
            self.highlighter.highlight(True)
        else:
            self.highlighter.highlight(False)
        if self.app.settings["warnings"]:
            self._run_linter()
        else:
            self.text.tag_remove("warn", "1.0", "end")

    def _run_linter(self):
        self.text.tag_remove("warn", "1.0", "end")
        code = self.text.get("1.0", "end-1c")
        self.warnings = Linter.check(code)
        t = self.app.theme
        self.text.tag_configure("warn", underline=True, underlinefg=t["warning"])
        for lineno, msg in self.warnings.items():
            start = f"{lineno}.0"
            end = f"{lineno}.end"
            self.text.tag_add("warn", start, end)

    def _show_warning(self, event):
        index = self.text.index(f"@{event.x},{event.y}")
        lineno = int(index.split(".")[0])
        msg = self.warnings.get(lineno)
        if msg:
            x = self.text.winfo_rootx() + event.x
            y = self.text.winfo_rooty() + event.y
            self.tooltip.show(msg, x, y)

    def apply_theme(self, theme):
        self.text.configure(bg=theme["editor_bg"], fg=theme["fg"])
        self.linenumbers.configure(bg=theme["editor_bg"])
        self.highlighter.set_theme(theme)
        self.highlighter._configure_tags()
        self._on_change()

    def set_language_from_path(self, path):
        if path:
            self.highlighter.detect_la
