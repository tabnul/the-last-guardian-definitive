#!/usr/bin/env python
"""Report for a frame timeline capture (documents/tlg/native plan/performance/frame-timeline-capture-design.md).

    python timeline_report.py <path of timeline_<tag>  (without .bin/.json)> [--html out.html] [--window-ms START] [--worst N]

Prints: frame times, CPU use per thread, GPU time by kind, the stage threads (front / raster / encoder), guest waits (who waits on what, who signals it) and
the frames where the time went. --html writes a Gantt chart (threads and the GPU on one time axis) of 6 frames around the worst frame, or from --window-ms
(milliseconds since the capture start).
"""
import argparse
import collections
import json
import sys
from pathlib import Path

import numpy as np

DT = np.dtype([('ns', '<u8'), ('arg', '<u8'), ('arg2', '<u8'), ('type', '<u4'), ('thread', '<u4')])
T_PRESENT_START, T_PRESENT_END = 6, 7
T_GAME_WAIT, T_GAME_SIGNAL = 10, 11
T_STAGE, T_GPU_CB, T_GPU_PASS, T_CPU, T_SYNC = 20, 21, 22, 23, 24
T_SPAN = 25
STAGE = {1: 'front op', 2: 'front: queue full', 3: 'front: drain writes', 4: 'raster op', 5: 'raster idle', 6: 'raster pass', 7: 'encoder replay',
         8: 'encoder submit', 9: 'encoder idle', 10: 'raster: handoff wait'}
WAIT_KIND = {1: 'cond', 2: 'sema', 3: 'sleep', 4: 'equeue', 5: 'mutex', 6: 'other'}
PASS_KIND = {0x01: 'between passes (before a pass)', 0x11: 'pass depth only', 0x12: 'pass color', 0x13: 'pass gbuffer (4+ targets)',
             0x20: 'between passes (before a dispatch)', 0x21: 'dispatch', 0xF0: 'out of order stamp',
             0x30: 'vkb submission (lean stamps)', 0x31: 'vkb staging copies'}
# GPU busy: passes and dispatches (PS4 path, or the backend with TLG_VKB_STAMPS=1); the backend's default lean stamps give whole submissions (0x30) and its
# staging copies (0x31)
BUSY_KINDS = [0x11, 0x12, 0x13, 0x21, 0x30, 0x31]


def load(base):
    meta = json.load(open(base + '.json'))
    raw = open(base + '.bin', 'rb').read()
    assert raw[:8] == b'TLTL0001', 'not a timeline capture'
    count, t0, t1 = np.frombuffer(raw, '<u8', 3, 8)
    ev = np.frombuffer(raw, DT, int(count), 32)
    return meta, ev, int(t0), int(t1)


def overlap_sum(starts, ends, a, b):
    """Total length of [starts, ends) inside [a, b); starts sorted."""
    s = np.clip(starts, a, b)
    e = np.clip(ends, a, b)
    return float(np.sum(e - s))


def pct(values, q):
    return float(np.percentile(values, q)) if len(values) else 0.0


def print_jobs(ev, nframes, tname):
    """Job level spans (hook span_job on FUN_01e4baf0): the engine's jobs grouped by their function."""
    sel = ev[(ev['type'] == T_SPAN) | (ev['type'] == 26)]
    if not len(sel):
        return
    info = {}
    groups = collections.defaultdict(list)
    for e in sel:
        th = int(e['thread'])
        if e['type'] == 26:
            info[th] = (int(e['arg']), int(e['arg2']))
        elif (int(e['arg']) >> 48) == 1000 and th in info:
            groups[info.pop(th)].append(int(e['arg']) & ((1 << 48) - 1))
    if not groups:
        return
    rows = []
    total = 0.0
    for (exec_fn, setup_fn), durs in groups.items():
        d = np.array(durs, dtype=np.float64) / 1e6
        total += float(d.sum())
        rows.append((float(d.sum()) / nframes, exec_fn, setup_fn, len(d) / nframes, float(d.mean()) * 1000, pct(d, 90) * 1000, float(d.max())))
    print('\n== engine jobs (hook span_job: jobs of at least TLG_JOB_MIN_US, grouped by job function; nested jobs are counted in the job that runs them)')
    print('   total %.2f ms of job time per frame over %d job kinds' % (total / nframes, len(rows)))
    for tot, ex, st, per, mean_us, p90_us, mx in sorted(rows, reverse=True)[:25]:
        print('   fn %#010x setup %#010x  %7.2f ms/frame  %7.1f jobs/frame  mean %8.1f us  p90 %8.1f us  max %7.2f ms' % (ex, st, tot, per, mean_us, p90_us, mx))


def print_critical_path(mean_dur):
    """Longest chain of dependent tasks by mean duration, from the task graph edges (documents/tlg/profiler/task_graph_edges.json)."""
    path = Path(__file__).resolve().parent.parent / 'profiler' / 'task_graph_edges.json'
    if not path.exists():
        return
    edges = json.load(open(path))['edges']  # [task, dependency]: task waits for dependency
    deps = collections.defaultdict(set)
    for a, b in edges:
        if a != b:
            deps[a].add(b)
    memo, best = {}, {}

    def finish(task, stack=()):
        if task in memo:
            return memo[task]
        if task in stack:
            return 0.0
        longest, pick = 0.0, None
        for dep in deps.get(task, ()):
            v = finish(dep, stack + (task,))
            if v > longest:
                longest, pick = v, dep
        memo[task] = longest + mean_dur.get(task, 0.0)
        best[task] = pick
        return memo[task]

    tasks = set(deps) | {d for ds in deps.values() for d in ds}
    if not tasks:
        return
    last = max(tasks, key=finish)
    chain = []
    while last:
        chain.append(last)
        last = best.get(last)
    chain.reverse()
    print('\n== critical path of the task graph by mean duration: %.2f ms (the graph edges are read from the decompile and may miss some)' % memo[chain[-1]])
    for task in chain:
        print('   %-36s %6.3f ms' % (task, mean_dur.get(task, 0.0)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('--html')
    ap.add_argument('--window-ms', type=float)
    ap.add_argument('--worst', type=int, default=8)
    args = ap.parse_args()
    meta, ev, t0, t1 = load(args.base)
    names = {int(k): v for k, v in meta['threads'].items()}
    objs = {int(k): v for k, v in meta['names'].items()}
    dur = (t1 - t0) / 1e9
    dup = collections.Counter(names.values())
    tname = lambda t: ('%s#%d' % (names[int(t)], t) if dup[names[int(t)]] > 1 else names[int(t)]) if int(t) in names else 'tid%d' % t
    print('== capture: %.2f s, %d events, %d lost' % (dur, len(ev), meta['lost_events']))
    if meta['lost_events']:
        print('   WARNING: the ring was full, the end of the capture is missing')

    # ---- frames
    ps = ev[(ev['type'] == T_PRESENT_START) & (ev['arg'] == 0)]['ns'].astype(np.int64)
    if len(ps) < 2:  # captures from builds without the present markers: the game's flip submits
        ps = ev[ev['type'] == 3]['ns'].astype(np.int64)
        print('(no present markers: frames are the flip submits)')
    ps.sort()
    frame_ms = np.diff(ps) / 1e6
    print('\n== frames: %d presented, %.1f fps' % (len(ps), (len(ps) - 1) / ((ps[-1] - ps[0]) / 1e9) if len(ps) > 1 else 0))
    if len(frame_ms):
        print('   frame ms: mean %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f' % (frame_ms.mean(), pct(frame_ms, 50), pct(frame_ms, 95), pct(frame_ms, 99), frame_ms.max()))

    nframes = max(len(ps) - 1, 1)

    # ---- CPU per thread
    cpu = ev[ev['type'] == T_CPU]
    cyc_ns = meta.get('cycles_per_ns') or 1.0
    print('\n== CPU use per thread (percent of one core, sampled 1 kHz)')
    per = collections.Counter()
    for t, a in zip(cpu['thread'], cpu['arg']):
        per[int(t)] += int(a)
    rows = sorted(per.items(), key=lambda kv: -kv[1])[:14]
    for t, c in rows:
        print('   %-42s %6.1f%%' % (tname(t), c / cyc_ns / (t1 - t0) * 100))
    print('   (total %.0f%% of one core)' % (sum(per.values()) / cyc_ns / (t1 - t0) * 100))

    # ---- GPU
    cb = ev[ev['type'] == T_GPU_CB]
    gp = ev[ev['type'] == T_GPU_PASS]
    cb_start = cb['ns'].astype(np.int64) - cb['arg'].astype(np.int64)
    cb_end = cb['ns'].astype(np.int64)
    order = np.argsort(cb_start)
    cb_start, cb_end = cb_start[order], cb_end[order]
    work = gp[np.isin(gp['arg'] >> 56, BUSY_KINDS)]
    w_dur = (work['arg'] & ((1 << 56) - 1)).astype(np.int64)
    w_end = work['ns'].astype(np.int64)
    o = np.argsort(w_end)
    w_end, w_dur = w_end[o], w_dur[o]
    w_start = w_end - w_dur
    busy = float(np.sum(w_dur)) / (t1 - t0) * 100
    cb_busy = float(np.sum(cb_end - cb_start)) / (t1 - t0) * 100 if len(cb) else 0
    print('\n== GPU: %d command buffers; passes and dispatches take %.1f%% of the capture (command buffer begin..end sums to %.1f%%)' % (len(cb), busy, cb_busy))
    nframes = max(len(ps) - 1, 1)
    kinds = collections.defaultdict(lambda: [0, 0])
    for a, n in zip(gp['arg'], gp['arg']):
        k = int(a) >> 56
        kinds[k][0] += 1
        kinds[k][1] += int(a) & ((1 << 56) - 1)
    for k, (n, ns) in sorted(kinds.items(), key=lambda kv: -kv[1][1]):
        print('   %-38s %7d x  %7.2f ms/frame  (%.1f%% of the capture)' % (PASS_KIND.get(k, hex(k)), n, ns / 1e6 / nframes, ns / (t1 - t0) * 100))
    big = [(int(a) & ((1 << 56) - 1), int(b)) for a, b in zip(gp['arg'], gp['arg2']) if (int(a) >> 56) in (0x11, 0x12, 0x13)]
    big.sort(reverse=True)
    if big:
        print('   biggest passes: ' + ', '.join('%.2f ms (%d draws, %dx%d)' % (d / 1e6, b & 0xffffff, (b >> 24) & 0xfffff, b >> 44) for d, b in big[:6]))

    # ---- stage spans
    sp = ev[ev['type'] == T_STAGE]
    print('\n== stage threads (ms per frame, and percent of the capture)')
    for st in sorted(set(int(a) >> 56 for a in sp['arg'])):
        sel = sp[(sp['arg'] >> 56) == st]
        total = int(np.sum(sel['arg'] & ((1 << 56) - 1)))
        thread = collections.Counter(int(t) for t in sel['thread']).most_common(1)[0][0]
        print('   %-24s %-28s %8.2f ms/frame  %5.1f%%  (%d spans)' % (STAGE.get(st, st), tname(thread), total / 1e6 / nframes, total / (t1 - t0) * 100, len(sel)))

    # ---- guest spans (the profiler's spans file: the engine's tasks)
    sp = ev[ev['type'] == T_SPAN]
    span_names = {int(k): v for k, v in meta.get('spans', {}).items()}
    mean_dur = {}
    if len(sp):
        print('\n== engine tasks (guest spans; ms per call, calls per frame, threads they ran on)')
        rows = []
        for sid in sorted(set(int(a) >> 48 for a in sp['arg'])):
            sel = sp[(sp['arg'] >> 48) == sid]
            d = (sel['arg'] & ((1 << 48) - 1)).astype(np.int64) / 1e6
            ths = collections.Counter(tname(int(x)) for x in sel['thread'])
            mean_dur[span_names.get(sid, str(sid)).replace('Task:', '')] = float(d.mean())
            rows.append((float(d.sum()) / nframes, span_names.get(sid, str(sid)), len(sel) / nframes, float(d.mean()), pct(d, 90), float(d.max()), ths))
        for total, name, per, mean, p90, mx, ths in sorted(rows, reverse=True)[:30]:
            print('   %-40s %7.2f ms/frame  %5.2f calls  mean %6.3f  p90 %6.3f  max %6.3f  on %s' % (name, total, per, mean, p90, mx, ', '.join('%s x%d' % kv for kv in ths.most_common(2))))
        print_critical_path(mean_dur)
    print_jobs(ev, nframes, tname)
    # ---- guest waits
    gw = ev[ev['type'] == T_GAME_WAIT]
    gs = ev[ev['type'] == T_GAME_SIGNAL]
    signalers = collections.defaultdict(collections.Counter)
    for o, t in zip(gs['arg2'], gs['thread']):
        signalers[int(o)][int(t)] += 1
    print('\n== guest waits (blocked time per thread: ms per frame, count)')
    by_thread = collections.defaultdict(lambda: [0, 0])
    by_rel = collections.defaultdict(lambda: [0, 0])
    for a, o, t in zip(gw['arg'], gw['arg2'], gw['thread']):
        ns = int(a) & ((1 << 56) - 1)
        kind = int(a) >> 56
        by_thread[int(t)][0] += ns
        by_thread[int(t)][1] += 1
        by_rel[(int(t), int(o), kind)][0] += ns
        by_rel[(int(t), int(o), kind)][1] += 1
    for t, (ns, n) in sorted(by_thread.items(), key=lambda kv: -kv[1][0])[:10]:
        print('   %-42s %8.2f ms/frame  %6d waits' % (tname(t), ns / 1e6 / nframes, n))
    print('\n== longest wait relations (waiter, object, kind, total blocked, who signals it)')
    for (t, o, kind), (ns, n) in sorted(by_rel.items(), key=lambda kv: -kv[1][0])[:14]:
        sig = ', '.join('%s x%d' % (tname(s), c) for s, c in signalers.get(o, collections.Counter()).most_common(2)) or 'nobody seen'
        print('   %-26s %-30s %-6s %8.2f ms/frame %6d x   <- %s' % (tname(t), objs.get(o, hex(o)), WAIT_KIND.get(kind, kind), ns / 1e6 / nframes, n, sig))

    # ---- per frame
    if len(ps) > 2:
        r_end = sp[(sp['arg'] >> 56) == 4]
        r_start = r_end['ns'].astype(np.int64) - (r_end['arg'] & ((1 << 56) - 1)).astype(np.int64)
        r_end_ns = r_end['ns'].astype(np.int64)
        f_sel = sp[(sp['arg'] >> 56) == 1]
        f_end = f_sel['ns'].astype(np.int64)
        f_start = f_end - (f_sel['arg'] & ((1 << 56) - 1)).astype(np.int64)
        e_sel = sp[(sp['arg'] >> 56) == 7]
        e_end = e_sel['ns'].astype(np.int64)
        e_start = e_end - (e_sel['arg'] & ((1 << 56) - 1)).astype(np.int64)
        print('\n== frames with the longest time (ms; busy = time inside the frame)')
        print('   %9s %7s %7s %7s %7s %7s   %s' % ('at ms', 'frame', 'gpu', 'raster', 'front', 'encoder', 'most busy'))
        picks = np.argsort(-frame_ms)[:args.worst]
        for i in sorted(picks):
            a, b = int(ps[i]), int(ps[i + 1])
            vals = {'gpu': overlap_sum(w_start, w_end, a, b), 'raster': overlap_sum(r_start, r_end_ns, a, b),
                    'front': overlap_sum(f_start, f_end, a, b), 'encoder': overlap_sum(e_start, e_end, a, b)}
            top = max(vals, key=vals.get)
            print('   %9.1f %7.2f %7.2f %7.2f %7.2f %7.2f   %s (%.0f%%)' % ((a - t0) / 1e6, (b - a) / 1e6, vals['gpu'] / 1e6, vals['raster'] / 1e6, vals['front'] / 1e6,
                                                                       vals['encoder'] / 1e6, top, vals[top] / (b - a) * 100))
        tot = {'gpu': overlap_sum(w_start, w_end, int(ps[0]), int(ps[-1])), 'raster': overlap_sum(r_start, r_end_ns, int(ps[0]), int(ps[-1])),
               'front': overlap_sum(f_start, f_end, int(ps[0]), int(ps[-1])), 'encoder': overlap_sum(e_start, e_end, int(ps[0]), int(ps[-1]))}
        span = float(ps[-1] - ps[0])
        print('\n== busy share over all frames: ' + ', '.join('%s %.0f%%' % (k, v / span * 100) for k, v in tot.items()))

    if args.html:
        write_html(args.html, meta, ev, t0, t1, ps, frame_ms, args, names, objs)


def write_html(path, meta, ev, t0, t1, ps, frame_ms, args, names, objs):
    if args.window_ms is not None:
        a = t0 + int(args.window_ms * 1e6)
    elif len(ps) > 2:
        i = int(np.argmax(frame_ms))
        a = int(ps[max(i - 2, 0)])
    else:
        a = t0
    b = a + int(6 * (np.median(frame_ms) if len(frame_ms) else 10) * 1e6)
    rows = collections.OrderedDict()
    spans = []  # [row, start_ms, end_ms, color, label]
    marks = []

    def row(name):
        rows.setdefault(name, len(rows))
        return rows[name]

    COL = {1: '#4c78a8', 2: '#e45756', 3: '#f58518', 4: '#54a24b', 5: '#c8c8c8', 6: '#2f7d32', 7: '#b279a2', 8: '#9d755d', 9: '#c8c8c8', 10: '#e45756'}
    WCOL = {1: '#d62728', 2: '#ff7f0e', 3: '#bbbbbb', 4: '#8c564b', 5: '#e377c2', 6: '#7f7f7f'}
    GCOL = {0x11: '#17becf', 0x12: '#1f77b4', 0x13: '#9467bd', 0x21: '#bcbd22', 0x01: '#dddddd', 0x20: '#dddddd'}
    sel = ev[(ev['ns'] >= a) & (ev['ns'] <= b + 50_000_000)]
    for e in sel:
        t = int(e['type'])
        ns = int(e['ns'])
        if t == T_STAGE:
            st = int(e['arg']) >> 56
            d = int(e['arg']) & ((1 << 56) - 1)
            if st in (5, 9) or d < 20_000:
                continue
            spans.append([row('%s (%s)' % (names.get(int(e['thread']), 'tid%d' % e['thread']), STAGE.get(st, st).split()[0])), (ns - d - a) / 1e6, (ns - a) / 1e6, COL.get(st, '#888'), STAGE.get(st, st)])
        elif t == T_GAME_WAIT:
            d = int(e['arg']) & ((1 << 56) - 1)
            if d < 200_000:
                continue
            kind = int(e['arg']) >> 56
            spans.append([row('wait: ' + names.get(int(e['thread']), 'tid%d' % e['thread'])), (ns - d - a) / 1e6, (ns - a) / 1e6, WCOL.get(kind, '#888'),
                          '%s %s' % (WAIT_KIND.get(kind, kind), objs.get(int(e['arg2']), hex(int(e['arg2']))))])
        elif t == T_SPAN:
            d = int(e['arg']) & ((1 << 48) - 1)
            if d < 30_000:
                continue
            sid = int(e['arg']) >> 48
            spans.append([row('task: %s' % names.get(int(e['thread']), 'tid%d' % e['thread'])), (ns - d - a) / 1e6, (ns - a) / 1e6, '#e8a33d',
                          '%s' % meta.get('spans', {}).get(str(sid), sid)])
        elif t == T_GPU_CB:
            d = int(e['arg'])
            spans.append([row('GPU command buffers'), (ns - d - a) / 1e6, (ns - a) / 1e6, '#555555', 'submission %d' % int(e['arg2'])])
        elif t == T_GPU_PASS:
            k = int(e['arg']) >> 56
            d = int(e['arg']) & ((1 << 56) - 1)
            if k in (0x01, 0x20, 0xF0) or d < 10_000:
                continue
            b2 = int(e['arg2'])
            spans.append([row('GPU passes'), (ns - d - a) / 1e6, (ns - a) / 1e6, GCOL.get(k, '#888'), '%s %d draws %dx%d' % (PASS_KIND.get(k), b2 & 0xffffff, (b2 >> 24) & 0xfffff, b2 >> 44)])
        elif t in (T_PRESENT_START, 3):
            marks.append([(ns - a) / 1e6, 'present' if t == T_PRESENT_START else 'submit+flip'])
    spans = [s for s in spans if s[2] >= 0 and s[1] <= (b - a) / 1e6 + 50]
    order = sorted(rows, key=lambda n: (0 if n.startswith('GPU') else 1 if 'tlg:' in n or 'shadPS4' in n else 2, n))
    remap = {rows[n]: i for i, n in enumerate(order)}
    for s in spans:
        s[0] = remap[s[0]]
    html = """<!doctype html><meta charset=utf-8><title>Frame timeline</title>
<style>body{margin:0;font:12px system-ui;background:#fff;color:#222}#c{display:block}#tip{position:fixed;background:#222;color:#fff;padding:3px 6px;border-radius:3px;pointer-events:none;display:none}</style>
<div style="padding:6px">wheel = zoom, drag = move. Window starts %.1f ms after the capture start.</div><canvas id=c></canvas><div id=tip></div>
<script>
const rows=%s, spans=%s, marks=%s, span_ms=%f;
const c=document.getElementById('c'),g=c.getContext('2d'),tip=document.getElementById('tip');
const L=250,RH=20;c.width=innerWidth;c.height=rows.length*RH+30;
let x0=0,x1=span_ms;
function X(ms){return L+(ms-x0)/(x1-x0)*(c.width-L-10)}
function draw(){g.clearRect(0,0,c.width,c.height);g.font='11px system-ui';
 rows.forEach((n,i)=>{g.fillStyle=i%%2?'#f6f6f6':'#fff';g.fillRect(0,30+i*RH,c.width,RH);g.fillStyle='#222';g.fillText(n.slice(0,40),4,30+i*RH+14)});
 const step=Math.pow(10,Math.floor(Math.log10((x1-x0)/6)));for(let t=Math.ceil(x0/step)*step;t<x1;t+=step){g.fillStyle='#999';g.fillRect(X(t),20,1,c.height);g.fillText(t.toFixed(step<1?2:0)+' ms',X(t)+2,14)}
 marks.forEach(m=>{g.fillStyle=m[1]=='present'?'#e00':'#06c';g.fillRect(X(m[0]),20,1,c.height)});
 spans.forEach(s=>{const a=X(s[1]),b=X(s[2]);if(b<L||a>c.width)return;g.fillStyle=s[3];g.fillRect(Math.max(a,L),32+s[0]*RH,Math.max(b-a,1),RH-4)})}
c.onwheel=e=>{e.preventDefault();const m=x0+(e.offsetX-L)/(c.width-L-10)*(x1-x0),f=e.deltaY>0?1.25:0.8;x0=m-(m-x0)*f;x1=m+(x1-m)*f;draw()};
let drag=null;c.onmousedown=e=>drag=e.offsetX;onmouseup=()=>drag=null;
c.onmousemove=e=>{if(drag!==null){const d=(e.offsetX-drag)/(c.width-L-10)*(x1-x0);x0-=d;x1-=d;drag=e.offsetX;draw()}
 const m=x0+(e.offsetX-L)/(c.width-L-10)*(x1-x0),r=Math.floor((e.offsetY-32)/RH);let h=spans.find(s=>s[0]==r&&m>=s[1]&&m<=s[2]);
 if(h){tip.style.display='block';tip.style.left=(e.clientX+10)+'px';tip.style.top=(e.clientY+10)+'px';tip.textContent=h[4]+' - '+(h[2]-h[1]).toFixed(3)+' ms'}else tip.style.display='none'};
draw();
</script>""" % ((a - t0) / 1e6, json.dumps(order), json.dumps(spans), json.dumps(marks), (b - a) / 1e6)
    open(path, 'w', encoding='utf-8').write(html)
    print('\nGantt chart written to', path)


if __name__ == '__main__':
    main()
