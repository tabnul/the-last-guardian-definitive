import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
cap_path = os.environ["RDOC_CAP"]; outp = os.environ["RDOC_OUT"]
cap, ctrl = open_cap(cap_path)
acts = []; walk(ctrl.GetRootActions(), acts)
sf = ctrl.GetStructuredFile()
f = open(outp, "w")
n = 0
for a in acts:
    if a.flags & rd.ActionFlags.Dispatch:
        ctrl.SetFrameEvent(a.eventId, True)
        st = ctrl.GetPipelineState()
        sh = st.GetShader(rd.ShaderStage.Compute)
        ro = st.GetReadOnlyResources(rd.ShaderStage.Compute)
        rw = st.GetReadWriteResources(rd.ShaderStage.Compute)
        refl = st.GetShaderReflection(rd.ShaderStage.Compute)
        f.write("eid %d dim %s ro %d rw %d entry %s name %s\n" % (a.eventId, a.dispatchDimension, len(ro), len(rw), refl.entryPoint if refl else None, a.GetName(sf)))
        n += 1
f.close()
print("dispatches", n)
ctrl.Shutdown(); cap.Shutdown()
os._exit(0)
