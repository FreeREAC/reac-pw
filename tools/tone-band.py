#!/usr/bin/env python3
"""Level of a steady tone on one channel, in a form neither end of the SENS range can fake.

Two estimators are wrong here, each at one end:

  * A SINGLE DFT BIN under-reads badly. The REAC path is repaced free-run against
    the graph clock, and the resampler smears a synthesised 1000.000 Hz tone into a
    pedestal tens of Hz wide: measured on this rig, the centre bin holds only 1.3 dB
    of the tone's power over a 3 s window and it takes +/-20 Hz to recover the last
    0.05 dB. A 3-bin sum scattered readings by up to 3 dB and made the sweep
    non-monotonic while the waveform PEAK marched cleanly 1 dB per step.

  * BROADBAND RMS over-reads wherever the preamp's own noise is loud, and at the top
    of the SENS range it is: at 0x37 the wideband noise measures ~15 dB under the
    tone, so an RMS reading there is high and the top of the curve bends for no
    physical reason.

So: power summed over a BAND (default +/-25 Hz) around the measured peak. Wide enough
to hold the whole smeared tone, narrow enough that wideband noise contributes ~27 dB
less than it does to RMS. The normalisation is Parseval-exact and asserted in code —
summing every bin must reproduce mean(v*v) — because an earlier windowed version
triple-counted the main lobe and reported more power than the signal contained.

Harmonic bands come back as THD beside it. A clipped reading is the exact shape of
the non-linearity under test, so it must never be able to pass silently.

  tools/tone-band.py capture.wav [channels] [freq]
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

def band(path, freq=1000.0, ch=1, bw=25.0, skip=0.2):
    fmt,data=read(path); nch,rate=fmt[1],fmt[2]
    a=np.frombuffer(data,dtype='<f4'); n=len(a)//nch
    v=a[:n*nch].reshape(n,nch)[:,ch-1].astype(np.float64)
    v=v[int(skip*rate):]
    n=(len(v)//rate)*rate                      # whole seconds -> integer Hz bins
    if n==0: raise SystemExit("capture too short")
    v=v[:n]
    S=np.fft.rfft(v)
    P=(np.abs(S)**2)*2.0/(n*n); P[0]/=2.0      # one-sided mean-square per bin
    if n%2==0: P[-1]/=2.0
    tot=float((v*v).mean())
    assert abs(P.sum()-tot) <= 1e-9*max(tot,1e-12)+1e-18, (P.sum(), tot)
    f=np.fft.rfftfreq(n,1.0/rate)
    k0=int(np.argmin(np.abs(f-freq))); w=int(60.0/(f[1]-f[0]))
    kpk=k0-w+int(np.argmax(P[k0-w:k0+w]))
    fpk=float(f[kpk])
    tone=float(P[np.abs(f-fpk)<=bw].sum())
    thd=sum(float(P[np.abs(f-fpk*h)<=bw].sum()) for h in (2,3,4,5))
    noise=max(tot-tone,1e-30)
    return dict(f=fpk, tone=db(tone), rms=db(tot), noise=db(noise),
                snr=db(tone)-db(noise), thd=db(thd)-db(tone),
                peak=20*float(np.log10(max(np.max(np.abs(v)),1e-12))), secs=n/rate)

def selftest():
    """A known-answer control: the estimator must recover a level it was given.

    Includes an OFF-BIN, frequency-drifting tone, because that is what the rig
    actually delivers and it is what broke the single-bin version."""
    import tempfile, os
    rate=48000; ok=True
    for f0,drift,lvl in ((1000.0,0.0,-20.0),(1000.7,0.0,-6.0),(999.99,4.0,-40.0)):
        t=np.arange(3*rate)/rate
        ph=2*np.pi*(f0*t+drift*np.sin(2*np.pi*0.7*t)/(2*np.pi*0.7))
        x=(10**(lvl/20.0))*np.sqrt(2)*np.sin(ph)      # amplitude for that RMS
        p=tempfile.mktemp(suffix='.wav')
        n=len(x)
        open(p,'wb').write(b'RIFF'+struct.pack('<I',36+n*4)+b'WAVEfmt '+
            struct.pack('<IHHIIHH',16,3,1,rate,rate*4,4,32)+b'data'+
            struct.pack('<I',n*4)+x.astype('<f4').tobytes())
        r=band(p); os.remove(p)
        err=r['tone']-lvl
        print("  f0=%.2f drift=%.1fHz  want %.2f dBFS  got %.2f  err %+.3f dB  %s"
              %(f0,drift,lvl,r['tone'],err,"ok" if abs(err)<0.05 else "FAIL"))
        ok = ok and abs(err)<0.05
    return ok

if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--selftest':
        sys.exit(0 if selftest() else 1)
    p=sys.argv[1]
    chs=[int(x) for x in sys.argv[2].split(',')] if len(sys.argv)>2 else [1]
    fq=float(sys.argv[3]) if len(sys.argv)>3 else 1000.0
    for c in chs:
        r=band(p,freq=fq,ch=c)
        print("ch%-3d f=%8.1f tone %8.2f dBFS  rms %8.2f  noise %8.2f  T/N %7.2f  thd %7.2f  peak %7.2f  (%.0fs)"
              %(c,r['f'],r['tone'],r['rms'],r['noise'],r['snr'],r['thd'],r['peak'],r['secs']))
