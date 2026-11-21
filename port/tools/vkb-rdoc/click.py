import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
d = x11.XOpenDisplay(sys.argv[1].encode())
for xs, ys in zip(sys.argv[2::2], sys.argv[3::2]):
    xt.XTestFakeMotionEvent(d, -1, int(xs), int(ys), 0); x11.XFlush(d); time.sleep(0.3)
    xt.XTestFakeButtonEvent(d, 1, 1, 0); x11.XFlush(d); time.sleep(0.1)
    xt.XTestFakeButtonEvent(d, 1, 0, 0); x11.XFlush(d); time.sleep(1)
