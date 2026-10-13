import numpy as np
p = r'D:\ChatGPT\PAERealtimeImaging\testdata\14.dat'
raw = np.memmap(p, dtype='<f8', mode='r', shape=(4000, 8300), order='F')
fs = 200e6
for name, off, cut in [('wl1', 301, 358), ('wl2', 302, 371)]:
    x = np.array(raw[:, off-1:8300:2], dtype=np.float32)
    x[:300 + (13 if name == 'wl2' else 0), :] = 0
    x = x[cut-1:, :]
    X = np.fft.rfft(x, axis=0)
    P = np.mean(np.abs(X) ** 2, axis=1)
    f = np.fft.rfftfreq(x.shape[0], 1 / fs) / 1e6
    print(name, 'Nt', x.shape[0])
    for b in [(0, 2), (2, 10), (10, 40), (40, 80), (80, 100)]:
        e = float(P[(f >= b[0]) & (f < b[1])].sum())
        print(' band', b, 'share', e / float(P.sum()))
    print(' peakMHz', float(f[np.argmax(P)]), 'centroidMHz', float((f * P).sum() / P.sum()))
