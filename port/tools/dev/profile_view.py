#!/usr/bin/env python
"""One page of graphs for a whole frame profile (F9 capture, timeline_<tag>.bin/.json), for the live view (threads-pc-port.ps1).

    python profile_view.py <path of timeline_<tag> without .bin/.json> <out.html>

Graphs over the whole capture: the frame time of every frame, the CPU use of the render pipeline threads and the game workers, and the GPU busy time, in
100 ms steps; below them the text report (timeline_report.py). The page is self-contained (inline SVG, no scripts from the network).
"""
import html
import io
import subprocess
import sys
from contextlib import redirect_stdout
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import timeline_report as tr  # noqa: E402

STEP_NS = 100_000_000
COLORS = {'raster thread': '#d9534f', 'command processor': '#f0ad4e', 'encoder thread': '#5bc0de', 'game render thread': '#5cb85c', 'game workers (sum / cores)': '#9370db',
          'GPU busy': '#337ab7'}


def series_svg(title, xs, lines, y_max, unit, guides=(), width=1100, height=220):
    """lines: list of (name, values) over xs (seconds)"""
    left, right, top, bottom = 50, 170, 24, 28
    w, h = width - left - right, height - top - bottom
    x_max = max(xs[-1], 1e-6) if len(xs) else 1
    sx = lambda x: left + x / x_max * w
    sy = lambda y: top + h - min(y, y_max) / y_max * h
    out = ['<svg width="%d" height="%d" xmlns="http://www.w3.org/2000/svg" style="background:#111">' % (width, height),
           '<text x="%d" y="16" fill="#ddd" font-size="13">%s</text>' % (left, html.escape(title))]
    for g, label in guides:
        y = sy(g)
        out.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="#444" stroke-dasharray="4 4"/>' % (left, left + w, y, y))
        out.append('<text x="%d" y="%.1f" fill="#888" font-size="11">%s</text>' % (left + w + 4, y + 4, html.escape(label)))
    for t in range(0, int(x_max) + 1):
        x = sx(t)
        out.append('<line x1="%.1f" x2="%.1f" y1="%d" y2="%d" stroke="#2a2a2a"/>' % (x, x, top, top + h))
        out.append('<text x="%.1f" y="%d" fill="#888" font-size="11" text-anchor="middle">%d s</text>' % (x, top + h + 16, t))
    for frac in (0, 0.5, 1):
        v = y_max * frac
        out.append('<text x="%d" y="%.1f" fill="#888" font-size="11" text-anchor="end">%.0f%s</text>' % (left - 4, sy(v) + 4, v, unit))
    for i, (name, values) in enumerate(lines):
        color = COLORS.get(name, '#ccc')
        pts = ' '.join('%.1f,%.1f' % (sx(x), sy(v)) for x, v in zip(xs, values))
        out.append('<polyline fill="none" stroke="%s" stroke-width="1.6" points="%s"/>' % (color, pts))
        mean = float(np.mean(values)) if len(values) else 0
        out.append('<text x="%d" y="%d" fill="%s" font-size="12">%s %.0f%s</text>' % (left + w + 8, top + 30 + 16 * i, color, html.escape(name), mean, unit))
    out.append('</svg>')
    return '\n'.join(out)


def main():
    base, out_path = sys.argv[1], sys.argv[2]
    meta, ev, t0, t1 = tr.load(base)
    names = {int(k): v for k, v in meta['threads'].items()}
    cyc_ns = meta.get('cycles_per_ns') or 1.0
    dur_s = (t1 - t0) / 1e9

    # frames
    ps = ev[(ev['type'] == tr.T_PRESENT_START) & (ev['arg'] == 0)]['ns'].astype(np.int64)
    if len(ps) < 2:
        ps = ev[ev['type'] == 3]['ns'].astype(np.int64)
    ps.sort()
    frame_ms = np.diff(ps) / 1e6
    frame_x = (ps[1:] - t0) / 1e9 if len(ps) > 1 else np.array([])
    fps = (len(ps) - 1) / ((ps[-1] - ps[0]) / 1e9) if len(ps) > 1 else 0

    # CPU use per 100 ms step
    steps = max(int((t1 - t0) // STEP_NS), 1)
    xs = (np.arange(steps) + 0.5) * STEP_NS / 1e9
    cpu = ev[ev['type'] == tr.T_CPU]
    # the PS4 path's stage threads, or the Vulkan backend's (recorder = resolver + recording threads; presenter): whichever ran
    groups = {'raster thread': ['shadPS4:GpuRaster', 'vkb:Recorder'], 'command processor': ['shadPS4:GpuCommandProcessor', 'vkb:Record1', 'vkb:Record2'],
              'encoder thread': ['shadPS4:GpuEncoder', 'shadPS4:PresentThread', 'tlg:Present'],
              'game render thread': ['ps4render']}
    cpu_lines = []
    idx = np.clip(((cpu['ns'].astype(np.int64) - t0) // STEP_NS).astype(np.int64), 0, steps - 1)
    for label, thread_names in groups.items():
        ids = [t for t, n in names.items() if n in thread_names]
        mask = np.isin(cpu['thread'], ids)
        busy = np.bincount(idx[mask], weights=cpu['arg'][mask].astype(np.float64), minlength=steps) / cyc_ns / STEP_NS * 100
        cpu_lines.append((label, busy))
    worker_ids = [t for t, n in names.items() if n.startswith('workerThread')]
    if worker_ids:
        mask = np.isin(cpu['thread'], worker_ids)
        busy = np.bincount(idx[mask], weights=cpu['arg'][mask].astype(np.float64), minlength=steps) / cyc_ns / STEP_NS * 100 / len(worker_ids)
        cpu_lines.append(('game workers (sum / cores)', busy))

    # GPU busy per 100 ms step (passes and dispatches)
    gp = ev[ev['type'] == tr.T_GPU_PASS]
    work = gp[np.isin(gp['arg'] >> 56, [0x11, 0x12, 0x13, 0x21])]
    w_dur = (work['arg'] & ((1 << 56) - 1)).astype(np.int64)
    w_end = work['ns'].astype(np.int64)
    gidx = np.clip(((w_end - t0) // STEP_NS).astype(np.int64), 0, steps - 1)
    gpu = np.bincount(gidx, weights=w_dur.astype(np.float64), minlength=steps) / STEP_NS * 100

    # the limit: the busiest pipeline thread
    means = {name: float(np.mean(v)) for name, v in cpu_lines if name != 'game workers (sum / cores)'}
    limit = max(means, key=means.get) if means else '?'

    # text report
    text = io.StringIO()
    old_argv = sys.argv
    sys.argv = ['timeline_report.py', base]
    try:
        with redirect_stdout(text):
            tr.main()
    except Exception as e:  # the graphs still count
        text.write('\n(report failed: %s)\n' % e)
    sys.argv = old_argv

    p95 = float(np.percentile(frame_ms, 95)) if len(frame_ms) else 0
    page = ['<!doctype html><html><head><meta charset="utf-8"><title>Frame profile</title>',
            '<style>body{background:#000;color:#ddd;font-family:Consolas,monospace;margin:16px} h1{font-size:18px} .k{color:#f0ad4e} pre{font-size:12px;color:#bbb}</style></head><body>',
            '<h1>Frame profile: %s</h1>' % html.escape(Path(base).name),
            '<p>%.1f s captured, <span class="k">%.1f fps</span>, frame %.1f ms mean, %.1f ms p95, worst %.1f ms. Busiest pipeline thread: <span class="k">%s %.0f%%</span>, GPU busy %.0f%%.</p>'
            % (dur_s, fps, float(np.mean(frame_ms)) if len(frame_ms) else 0, p95, float(np.max(frame_ms)) if len(frame_ms) else 0, limit, means.get(limit, 0), float(np.mean(gpu))),
            series_svg('frame time per frame (ms)', frame_x, [('frame time', frame_ms)], max(50.0, p95 * 1.2), ' ms',
                       guides=((8.33, '120 fps'), (16.67, '60 fps'), (33.33, '30 fps'))),
            series_svg('CPU use of the pipeline threads (percent of one core, 100 ms steps)', xs, cpu_lines, 100, '%'),
            series_svg('GPU busy (passes and dispatches, 100 ms steps)', xs, [('GPU busy', gpu)], 100, '%'),
            '<pre>%s</pre>' % html.escape(text.getvalue()),
            '</body></html>']
    Path(out_path).write_text('\n'.join(page), encoding='utf-8')
    print('wrote %s' % out_path)


if __name__ == '__main__':
    main()
