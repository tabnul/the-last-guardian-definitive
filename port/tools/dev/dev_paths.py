"""The folders of this repo and the machine's own paths, for the dev tools here (import dev_paths).

Everything is found from this file's place: DEV (port/tools/dev), PORT (port/), REPO (the root), USER (TLG_USER_DIR, else port/user),
OUT (port/out), LOG (USER/log), RUNS (LOG/runs). The machine's paths come from paths.local.json next to this file (git-ignored; copy
paths.example.json): CFG['game_dir'], CFG['save_backups'], CFG['msys2_root'], CFG['saves'].
"""
import json
import os
from pathlib import Path

DEV = Path(__file__).resolve().parent
PORT = DEV.parent.parent
REPO = PORT.parent
USER = Path(os.environ['TLG_USER_DIR']) if os.environ.get('TLG_USER_DIR') else PORT / 'user'
OUT = PORT / 'out'
LOG = USER / 'log'
RUNS = LOG / 'runs'
DEBUG_PORT = int(os.environ.get('TLG_DEBUG_SERVER_PORT', '7777'))
BASE = f'http://127.0.0.1:{DEBUG_PORT}'

_cfg_file = DEV / 'paths.local.json'
CFG = json.loads(_cfg_file.read_text(encoding='utf-8-sig')) if _cfg_file.exists() else {}


def msys2_bin(tool):
    """A CLANG64 tool of the machine's MSYS2 (msys2_root in paths.local.json, else C:/msys64)."""
    root = Path(CFG.get('msys2_root') or 'C:/msys64')
    return str(root / 'clang64' / 'bin' / tool)
