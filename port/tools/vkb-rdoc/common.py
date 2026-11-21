import renderdoc as rd, os, sys
def open_cap(path):
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    r = cap.OpenFile(path, '', None)
    if r != rd.ResultCode.Succeeded: print("open fail", r); os._exit(2)
    r, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if r != rd.ResultCode.Succeeded: print("replay fail", r); os._exit(3)
    return cap, ctrl
def walk(actions, out):
    for a in actions:
        out.append(a); walk(a.children, out)
