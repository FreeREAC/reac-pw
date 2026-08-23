#!/usr/bin/env python3
"""Level of a steady tone, immune to BOTH failure modes this loop presents.

A single DFT bin under-reads when the received tone sits off the generated
frequency. Broadband RMS over-reads when the preamp's own noise is loud, which
at the top of the SENS range it is: at 0x37 the noise sits ~3 dB under the tone.
Neither alone is a gain probe.

So: BAND POWER over a few bins around the measured peak, normalised by Parseval
so that summing every bin reproduces mean(v*v) exactly (asserted below). Noise is
then the rest of the spectrum, and the tone-to-noise margin is visible at every
step. THD comes from the harmonic bands, so a clipped reading cannot pass itself
off as a compressed curve.
"""
import struct, sys, numpy as np

def read(path):
    d=open(path,'rb').read(); i=12; fmt=None; data=None
    while i+8<=len(d):
        cid,sz=d[i:i+4],struct.unpack('<I',d[i+4:i+8])[0]
        if cid==b'fmt ': fmt=struct.unpack('<HHIIHH',d[i+8:i+24])
        elif cid==b'data': data=d[i+8:i+8+sz]
        i+=8+sz+(sz&1)
    return fmt,data

def db(p): return -999.0 if p<=0 else 10*float(np.log10(p))

def band(path, freq=1000.0, ch=1, nbin=3, skip=0.4):
    fmt,data=read(path); nch,rate=fmt[1],fmt[2]
    a=np.frombuffer(data,dtype='<f4'); n=len(a)//nch
    v=a[:n*nch].reshape(n,nch)[:,ch-1].astype(np.float64)
    v=v[int(skip*rate):]
    n=(len(v)//rate)*rate                    # whole seconds -> 1 Hz bins
    if n==0: raise SystemExit("capture too short")
    v=v[:n]
    S=np.fft.rfft(v)
    # one-sided mean-square per bin; sum over all bins == mean(v*v)
    P=(np.abs(S)**2)*2.0/(n*n); P[0]/=2.0
    if n%2==0: P[-1]/=2.0
    tot=float((v*v).mean())
    assert abs(P.sum()-tot) < 1e-9*max(tot,1e-12)+1e-18, (P.sum(), tot)
    f=np.fft.rfftfreq(n,1.0/rate)
    k0=int(np.argmin(np.abs(f-freq))); w=int(60.0/(f[1]-f[0]))
    kpk=k0-w+int(np.argmax(P[k0-w:k0+w]))
    sl=slice(max(kpk-nbin,0),kpk+nbin+1)
    tone=float(P[sl].sum())
    thd=0.0
    for h in (2,3,4,5):
        kh=int(round(f[kpk]*h/(f[1]-f[0])))
        if kh+nbin<len(P): thd+=float(P[kh-nbin:kh+nbin+1].sum())
    noise=max(tot-tone,1e-30)
    return dict(f=float(f[kpk]), tone=db(tone), rms=db(tot), noise=db(noise),
                snr=db(tone)-db(noise), thd=db(thd)-db(tone),
                peak=20*float(np.log10(max(np.max(np.abs(v)),1e-12))), secs=n/rate)

if __name__=='__main__':
    p=sys.argv[1]; chs=[int(x) for x in sys.argv[2].split(',')] if len(sys.argv)>2 else [1]
    for c in chs:
        r=band(p,ch=c)
        print("ch%-3d f=%8.3f tone %8.2f dBFS  noise %8.2f  T/N %7.2f  thd %7.2f  peak %7.2f  (%.0fs)"
              %(c,r['f'],r['tone'],r['noise'],r['snr'],r['thd'],r['peak'],r['secs']))
