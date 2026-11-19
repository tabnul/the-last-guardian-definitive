import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
cap, ctrl = open_cap(os.environ["RDOC_CAP"])
want = os.environ.get("RDOC_DIM", "240,135,1")
acts = []; walk(ctrl.GetRootActions(), acts)
tex = {t.resourceId: t for t in ctrl.GetTextures()}
buf = {b.resourceId: b for b in ctrl.GetBuffers()}
f = open(os.environ["RDOC_OUT"], "w")
def d(rid):
    if rid in tex:
        t = tex[rid]; return "tex %dx%dx%d arr%d mips%d cube%s fmt %s" % (t.width, t.height, t.depth, t.arraysize, t.mips, t.cubemap, t.format.Name())
    if rid in buf: return "buf %d" % buf[rid].length
    return "?"
for a in acts:
    if a.flags & rd.ActionFlags.Dispatch and ",".join(str(x) for x in a.dispatchDimension) == want:
        ctrl.SetFrameEvent(a.eventId, True)
        st = ctrl.GetPipelineState()
        f.write("== eid %d\n" % a.eventId)
        for kind, lst in (("RO", st.GetReadOnlyResources(rd.ShaderStage.Compute)), ("RW", st.GetReadWriteResources(rd.ShaderStage.Compute))):
            for r in lst:
                rid = r.descriptor.resource
                f.write("  %s bind %d arr %d %s %s\n" % (kind, r.access.index if hasattr(r.access, 'index') else -1, r.access.arrayElement, rid, d(rid)))
f.close()
os._exit(0)
