import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
cap, ctrl = open_cap(os.environ["RDOC_CAP"])
eid = int(os.environ["RDOC_EID"]); outd = os.environ["RDOC_OUTDIR"]
os.makedirs(outd, exist_ok=True)
tex = {t.resourceId: t for t in ctrl.GetTextures()}
buf = {b.resourceId: b for b in ctrl.GetBuffers()}
ctrl.SetFrameEvent(eid, True)
st = ctrl.GetPipelineState()
meta = []
seen = set()
for kind, lst in (("RO", st.GetReadOnlyResources(rd.ShaderStage.Compute)), ("RW", st.GetReadWriteResources(rd.ShaderStage.Compute))):
    for r in lst:
        rid = r.descriptor.resource
        bind = r.access.index
        key = (kind, bind)
        base = "%s%02d" % (kind.lower(), bind)
        if rid in tex:
            t = tex[rid]
            data = b""
            nslices = max(t.arraysize, 1) if t.depth <= 1 else 1
            for s in range(nslices):
                data += ctrl.GetTextureData(rid, rd.Subresource(0, s, 0))
            fn = "%s_tex_%dx%dx%d_arr%d_%s.bin" % (base, t.width, t.height, t.depth, t.arraysize, t.format.Name())
            open(os.path.join(outd, fn), "wb").write(data)
            meta.append(dict(kind=kind, bind=bind, id=str(rid), type="tex", file=fn, w=t.width, h=t.height, d=t.depth, arr=t.arraysize, mips=t.mips, fmt=t.format.Name(), bytes=len(data), mip0_all_slices=True))
        elif rid in buf:
            b = buf[rid]
            off = r.descriptor.byteOffset; sz = r.descriptor.byteSize
            if sz == 0 or sz > b.length - off: sz = b.length - off
            data = ctrl.GetBufferData(rid, off, sz)
            fn = "%s_buf_off%d_size%d.bin" % (base, off, sz)
            open(os.path.join(outd, fn), "wb").write(data)
            meta.append(dict(kind=kind, bind=bind, id=str(rid), type="buf", file=fn, length=b.length, offset=off, size=sz, bytes=len(data)))
        else:
            meta.append(dict(kind=kind, bind=bind, id=str(rid), type="unknown"))
json.dump(dict(eid=eid, resources=meta), open(os.path.join(outd, "index.json"), "w"), indent=1)
print("done", len(meta))
os._exit(0)
