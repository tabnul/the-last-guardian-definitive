"""MCP server (stdio, JSON-RPC 2.0, no dependencies) that reads live data from the running game.

Works with the TLG PC port (tlg-probe.exe, started by port/tools/dev/play_start.ps1 with
TLG_DEBUG_SERVER_PORT=7777) and with the older shadPS4 emulator build (shadps4.exe, setting
debug_server_port in the game's custom config). Both answer JSON on http://127.0.0.1:<port>.

Register with Claude Code (project scope, from the project folder):
    claude mcp add shadps4 -s local -- python "<repo>\\port\\tools\\dev\\shadps4_mcp.py"
The port defaults to 7777; override with the environment variable SHADPS4_DEBUG_PORT.

2026-11-10 (port update): profile names come from llvm-symbolizer on tlg-probe.exe (DWARF); thread CPU is
measured from Windows thread times over a given number of seconds (the port's /threads endpoint gave
windows under one second, design review section 7); /cond does not exist in the port and was removed;
shadps4_get reads any endpoint ("/" lists them).
"""
import bisect
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402

PORT = int(os.environ.get('SHADPS4_DEBUG_PORT', '7777'))
BASE = f'http://127.0.0.1:{PORT}'
# old emulator build: MSVC linker map + undname
MAP_FILE = os.environ.get('SHADPS4_MAP')
UNDNAME = os.environ.get(
    'SHADPS4_UNDNAME',
    'undname')
IMAGE_BASE = 0x700000000000
# PC port: DWARF in the exe, resolved with llvm-symbolizer
SYMBOLIZER = os.environ.get('TLG_SYMBOLIZER') or dev_paths.msys2_bin('llvm-symbolizer.exe')
PORT_EXE_FALLBACK = str(dev_paths.OUT / 'tlg-probe.exe')
PROCESS_NAMES = ('tlg-probe', 'shadps4')

TOOLS = [
    {
        'name': 'shadps4_status',
        'description': 'Uptime, presented frames and fps (last 1 s and 5 s) of the running game.',
        'path': '/status',
        'params': {},
    },
    {
        'name': 'shadps4_threads',
        'description': 'CPU use (100 = one core) of the busiest game threads over the next few seconds, '
                       'measured from Windows thread times; names from the game (/timeline).',
        'path': None,
        'params': {'seconds': 'measuring time, 1..30 (default 5)', 'top': 'threads to list (default 16)'},
        'local': 'threads',
    },
    {
        'name': 'shadps4_stats',
        'description': 'Latest published counters (/stats): command processor split, raster thread, GPU time, '
                       'and every group the port publishes.',
        'path': '/stats',
        'params': {},
    },
    {
        'name': 'shadps4_hooks',
        'description': 'Calls per frame and totals of the installed guest hooks (needs a CUSA03627.hooks.json).',
        'path': '/hooks',
        'params': {},
        'via': '/stats',
        'key': 'hooks',
    },
    {
        'name': 'shadps4_faults',
        'description': 'CPU write faults on page-tracked guest memory since the previous call, grouped by 4 MB region.',
        'path': '/faults',
        'params': {'top': 'number of regions to list (default 15)'},
    },
    {
        'name': 'shadps4_memory',
        'description': 'Reads guest memory (hex, plus u32 words when size <= 256 and a multiple of 4). '
                       'Guest addresses are runtime addresses; ghidra = runtime - eboot_base + 0x1000000 '
                       '(or pass ghidra=0x... instead of addr).',
        'path': '/memory',
        'params': {'addr': 'guest address, e.g. 0x28c30000', 'ghidra': 'Ghidra address of the game image instead of addr',
                   'size': 'bytes, 1..65536 (default 64)'},
    },
    {
        'name': 'shadps4_profile',
        'description': 'Samples the call stack of one game thread (default the raster thread) for a few seconds and '
                       'returns the busiest functions: self % and inclusive %. Names from llvm-symbolizer (PC port) '
                       'or the linker map (old emulator). The thread slows down slightly while sampled.',
        'path': '/profile',
        'params': {'thread': 'thread name part (default GpuRaster; others: GpuCommandProcessor, ps4render)',
                   'seconds': 'sampling time, 1..30 (default 5)',
                   'hz': 'samples per second (default 500)',
                   'top': 'functions per list (default 25)'},
        'symbolize': True,
    },
    {
        'name': 'shadps4_log',
        'description': 'Tail of a log file of the game, optionally filtered by a substring.',
        'path': '/log',
        'params': {'lines': 'number of lines (default 50, max 2000)',
                   'filter': 'only lines containing this text',
                   'file': 'log file name (default shad_log.txt)'},
    },
    {
        'name': 'shadps4_get',
        'description': 'Reads any endpoint of the game\'s debug server, e.g. /stats, /frame_gate, '
                       '/native, /invariants, /resources. (/hook_ring, /ring_census, /updates and /uploads were removed 2026-11-20, note 105.) path "/" lists every endpoint with its description. '
                       'Only read endpoints: /set and other switches are refused here.',
        'path': None,
        'params': {'path': 'endpoint path, e.g. /frame_gate', 'query': 'optional query string, e.g. n=200'},
        'required': ['path'],
        'local': 'get',
    },
]

# shadps4_get refuses the endpoints that change the running game (owner rule: no live switches while testing): /set, /capture,
# /timeline_capture, /prof with arguments, /resources?start, /registry?start, /flight?bench


_symbols = None
_symbols_key = None


def running_exe():
    """(process name, exe path) of the running game, or (None, None)."""
    for name in PROCESS_NAMES:
        try:
            out = subprocess.run(
                ['powershell', '-NoProfile', '-Command', f'(Get-Process {name} -ErrorAction Stop | Select-Object -First 1).Path'],
                capture_output=True, text=True, timeout=15).stdout.strip()
            if out:
                return name, out
        except Exception:
            pass
    return None, None


def find_map_file():
    """The linker map of the old emulator: shadps4.map next to its exe; SHADPS4_MAP overrides; MAP_FILE is the fallback."""
    if 'SHADPS4_MAP' in os.environ:
        return os.environ['SHADPS4_MAP']
    name, path = running_exe()
    if name == 'shadps4' and path:
        candidate = os.path.join(os.path.dirname(path), 'shadps4.map')
        if os.path.exists(candidate):
            return candidate
    return MAP_FILE


def load_symbols():
    global _symbols, _symbols_key
    map_file = find_map_file()
    key = (map_file, os.path.getmtime(map_file))
    if _symbols is None or _symbols_key != key:
        _symbols_key = key
        rvas, names = [], []
        pattern = re.compile(r'^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s')
        with open(map_file, errors='replace') as f:
            for line in f:
                m = pattern.match(line)
                if m:
                    rvas.append(int(m.group(2), 16) - IMAGE_BASE)
                    names.append(m.group(1))
        order = sorted(range(len(rvas)), key=rvas.__getitem__)
        _symbols = ([rvas[i] for i in order], [names[i] for i in order])
    return _symbols


def demangle(names):
    result = {}
    todo = [n for n in dict.fromkeys(names) if n.startswith('?')]
    for i in range(0, len(todo), 15):
        chunk = todo[i:i + 15]
        out = subprocess.run([UNDNAME, '0x1000'] + chunk, capture_output=True, text=True).stdout
        decoded = re.findall(r'^is :- "(.*)"$', out, re.M)
        if len(decoded) == len(chunk):
            result.update(zip(chunk, decoded))
    return result


def symbolize_port(rvas, exe):
    """Function names of RVAs in the PC port's exe (DWARF), one per RVA."""
    if not rvas:
        return []
    args = [SYMBOLIZER, '--obj=' + exe, '--relative-address', '--demangle', '--no-inlines', '--functions=short']
    out = subprocess.run(args, input='\n'.join(rvas) + '\n', capture_output=True, text=True, timeout=120).stdout.split('\n\n')
    names = []
    for block in out:
        lines = [l for l in block.strip().split('\n') if l]
        names.append(lines[0] if lines else '?')
    names += ['?'] * (len(rvas) - len(names))
    return names[:len(rvas)]


def symbolize(profile):
    entries = [e for key in ('top_self', 'top_inclusive') for e in profile.get(key, [])]
    for group in profile.get('time_outside_exe', {}).values():
        if isinstance(group, dict):
            entries += group.get('callers', [])
    name, exe = running_exe()
    port_rows = [e for e in entries if e.get('module', '').lower() == 'tlg-probe.exe']
    if port_rows:
        exe = exe if name == 'tlg-probe' and exe else PORT_EXE_FALLBACK
        names = symbolize_port([e['rva'] for e in port_rows], exe)
        for e, n in zip(port_rows, names):
            e['function'] = n
    emu_rows = [e for e in entries if e.get('module', '').lower() == 'shadps4.exe']
    if emu_rows and find_map_file():
        rvas, map_names = load_symbols()
        found = {}
        for e in emu_rows:
            i = bisect.bisect_right(rvas, int(e['rva'], 16)) - 1
            found[id(e)] = map_names[i] if i >= 0 else '?'
        pretty = demangle(list(found.values()))
        for e in emu_rows:
            raw = found[id(e)]
            e['function'] = pretty.get(raw, raw)
    for e in entries:
        if 'function' not in e:
            e['function'] = e.get('module', '?') + '+' + e.get('rva', '?')
        for k in ('module', 'rva'):
            e.pop(k, None)
        for k in ('self_pct', 'inclusive_pct'):
            if k in e:
                e[k] = round(e[k], 1)
    return profile


def http_get(path_and_query, timeout):
    try:
        with urllib.request.urlopen(BASE + path_and_query, timeout=timeout) as response:
            return json.loads(response.read().decode('utf-8'))
    except urllib.error.URLError as e:
        raise RuntimeError(f'cannot reach the game at {BASE} ({e.reason}). Is it running with the debug server on port {PORT} '
                           f'(TLG_DEBUG_SERVER_PORT for the PC port, debug_server_port for the old emulator)?')


def measure_threads(arguments):
    seconds = min(max(float(arguments.get('seconds') or 5), 1.0), 30.0)
    top = int(arguments.get('top') or 16)
    names = {}
    try:
        names = {int(k): v for k, v in http_get('/timeline?n=1', 15).get('threads', {}).items()}
    except Exception:
        pass
    script = (
        "$p = $null; foreach ($n in @('tlg-probe','shadps4')) { $p = Get-Process -Name $n -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p) { break } }; "
        "if (-not $p) { '{}'; exit }; "
        "$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }; "
        "$sw = [Diagnostics.Stopwatch]::StartNew(); Start-Sleep -Milliseconds " + str(int(seconds * 1000)) + "; $p.Refresh(); $wall = $sw.Elapsed.TotalMilliseconds; "
        "$out = @(); foreach ($t in $p.Threads) { try { if ($a.ContainsKey($t.Id)) { $out += [pscustomobject]@{ tid = $t.Id; ms = $t.TotalProcessorTime.TotalMilliseconds - $a[$t.Id] } } } catch {} }; "
        "@{ process = $p.ProcessName; wall_ms = $wall; threads = $out } | ConvertTo-Json -Depth 3 -Compress")
    out = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, timeout=seconds + 60).stdout.strip()
    data = json.loads(out or '{}')
    if not data:
        raise RuntimeError('no game process (tlg-probe or shadps4) is running')
    wall = data['wall_ms'] or 1.0
    rows = data.get('threads') or []
    if isinstance(rows, dict):
        rows = [rows]
    rows = [{'name': names.get(r['tid'], f"tid{r['tid']}"), 'tid': r['tid'], 'percent': round(100.0 * r['ms'] / wall, 1)} for r in rows]
    rows.sort(key=lambda r: -r['percent'])
    return {'note': 'percent of one core over the measured time, from Windows thread times', 'process': data['process'],
            'seconds': round(wall / 1000.0, 2), 'total_percent': round(sum(r['percent'] for r in rows), 1), 'threads': rows[:top]}


def get_endpoint(arguments):
    path = str(arguments.get('path') or '/').strip()
    if not path.startswith('/'):
        path = '/' + path
    query = str(arguments.get('query') or '').lstrip('?')
    full = path + ('?' + query if query else '')
    if path in ('/set', '/capture', '/timeline_capture') or (path == '/prof' and query) or any(full.startswith(p) for p in ('/resources?start', '/registry?start', '/flight?bench')):
        raise RuntimeError(f'{path} changes the running game; it is not called from this tool')
    return http_get(full, 60)


def call_endpoint(tool, arguments):
    if tool.get('local') == 'threads':
        return measure_threads(arguments)
    if tool.get('local') == 'get':
        return get_endpoint(arguments)
    path = tool.get('via', tool['path'])
    query = {k: str(v) for k, v in arguments.items() if k in tool['params'] and v not in (None, '')}
    if tool['name'] == 'shadps4_memory' and 'addr' not in query and 'ghidra' not in query:
        raise RuntimeError('give addr or ghidra')
    wait = 15 + (float(arguments.get('seconds', 5)) if tool.get('symbolize') else 0)
    data = http_get(path + ('?' + urllib.parse.urlencode(query) if query else ''), wait)
    if tool.get('symbolize') and 'top_self' in data:
        data = symbolize(data)
    if 'key' in tool:
        data = data.get(tool['key'], {'note': 'nothing published yet (no hooks installed?)'})
    return data


def tool_schema(tool):
    props = {k: {'type': 'string', 'description': d} for k, d in tool['params'].items()}
    return {
        'name': tool['name'],
        'description': tool['description'],
        'inputSchema': {'type': 'object', 'properties': props, 'required': tool.get('required', [])},
    }


def handle(message):
    method = message.get('method')
    params = message.get('params') or {}
    if method == 'initialize':
        return {
            'protocolVersion': params.get('protocolVersion', '2024-11-05'),
            'capabilities': {'tools': {}},
            'serverInfo': {'name': 'shadps4', 'version': '1.1'},
        }
    if method == 'tools/list':
        return {'tools': [tool_schema(t) for t in TOOLS]}
    if method == 'tools/call':
        tool = next((t for t in TOOLS if t['name'] == params.get('name')), None)
        if tool is None:
            raise KeyError(f"unknown tool {params.get('name')}")
        try:
            data = call_endpoint(tool, params.get('arguments') or {})
            return {'content': [{'type': 'text', 'text': json.dumps(data, indent=1)}]}
        except Exception as e:  # reported to the model as a tool error, not a protocol error
            return {'content': [{'type': 'text', 'text': str(e)}], 'isError': True}
    if method == 'ping':
        return {}
    raise NotImplementedError(method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        message = json.loads(line)
        if 'id' not in message:  # notification
            continue
        try:
            reply = {'jsonrpc': '2.0', 'id': message['id'], 'result': handle(message)}
        except NotImplementedError as e:
            reply = {'jsonrpc': '2.0', 'id': message['id'],
                     'error': {'code': -32601, 'message': f'method not found: {e}'}}
        except Exception as e:
            reply = {'jsonrpc': '2.0', 'id': message['id'], 'error': {'code': -32603, 'message': str(e)}}
        sys.stdout.write(json.dumps(reply) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
