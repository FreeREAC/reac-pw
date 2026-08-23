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

A third thing can fake the curve on this rig and did: the master's pacer trims its
ring about once every twelve seconds, dropping ~64 ms out of the OUTBOUND tone. A
capture that contains one reads low by an amount that depends on where the gap fell,
which scattered the sweep by +/-0.3 dB and put false steps at arbitrary places. So the
level is taken over the longest run whose short-term envelope is FLAT — the glitch is
excluded rather than averaged in, and a capture with no steady run is refused instead
of returning a number.

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

def steadiest(v, rate, hop=0.025, tol_db=0.25, want=1.0):
    """The longest run whose short-term envelope holds within tol_db of its own
    median. Returns (start, stop) sample indices, or None if no run reaches `want`
    seconds -- a refusal, because a glitched capture must not silently return a
    level."""
    h=int(hop*rate); m=len(v)//h
    if m<3: return (0,len(v))
    e=np.sqrt((v[:m*h].reshape(m,h)**2).mean(axis=1))
    e=np.maximum(e,1e-12)
    med=np.median(e)
    ok=np.abs(20*np.log10(e/med))<=tol_db
    best=(0,0); i=0
    while i<m:
        if not ok[i]: i+=1; continue
        j=i
        while j<m and ok[j]: j+=1
        if j-i>best[1]-best[0]: best=(i,j)
        i=j
    if (best[1]-best[0])*hop < want: return None
    return (best[0]*h, best[1]*h)

def band(path, freq=1000.0, ch=1, bw=25.0, skip=0.2, steady=True):
    fmt,data=read(path); nch,rate=fmt[1],fmt[2]
    a=np.frombuffer(data,dtype='<f4'); n=len(a)//nch
    v=a[:n*nch].reshape(n,nch)[:,ch-1].astype(np.float64)
    v=v[int(skip*rate):]
    if steady:
        w=steadiest(v,rate)
        if w is None: return None
        v=v[w[0]:w[1]]
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
    for f0,drift,lvl,gap in ((1000.0,0.0,-20.0,0),(1000.7,0.0,-6.0,0),
                             (999.99,4.0,-40.0,0),(1000.0,0.0,-20.0,1)):
        t=np.arange(6*rate)/rate
        ph=2*np.pi*(f0*t+drift*np.sin(2*np.pi*0.7*t)/(2*np.pi*0.7))
        x=(10**(lvl/20.0))*np.sqrt(2)*np.sin(ph)      # amplitude for that RMS
        if gap:      # a pacer trim: ~64 ms of the tone missing mid-capture
            x[int(2.5*rate):int(2.564*rate)]=0.0
        p=tempfile.mktemp(suffix='.wav')
        n=len(x)
        open(p,'wb').write(b'RIFF'+struct.pack('<I',36+n*4)+b'WAVEfmt '+
            struct.pack('<IHHIIHH',16,3,1,rate,rate*4,4,32)+b'data'+
            struct.pack('<I',n*4)+x.astype('<f4').tobytes())
        r=band(p); os.remove(p)
        err=r['tone']-lvl
        print("  f0=%.2f drift=%.1fHz gap=%d  want %.2f dBFS  got %.2f  err %+.3f dB  %s"
              %(f0,drift,gap,lvl,r['tone'],err,"ok" if abs(err)<0.05 else "FAIL"))
        ok = ok and abs(err)<0.05
    # The gap detector must also be able to REFUSE: a capture that is glitch all
    # the way through has no steady run and must return nothing, not a number.
    x=(10**(-20/20.0))*np.sqrt(2)*np.sin(2*np.pi*1000*np.arange(3*rate)/rate)
    x[::4800]=0.0
    x*= (1+0.5*np.sin(2*np.pi*13*np.arange(3*rate)/rate))
    p=tempfile.mktemp(suffix='.wav'); n=len(x)
    open(p,'wb').write(b'RIFF'+struct.pack('<I',36+n*4)+b'WAVEfmt '+
        struct.pack('<IHHIIHH',16,3,1,rate,rate*4,4,32)+b'data'+
        struct.pack('<I',n*4)+x.astype('<f4').tobytes())
    r=band(p); os.remove(p)
    print("  amplitude-modulated throughout: %s"%("refused, ok" if r is None else "RETURNED %.2f dBFS -- FAIL"%r['tone']))
    ok = ok and r is None
    return ok

if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--selftest':
        sys.exit(0 if selftest() else 1)
    p=sys.argv[1]
    chs=[int(x) for x in sys.argv[2].split(',')] if len(sys.argv)>2 else [1]
    fq=float(sys.argv[3]) if len(sys.argv)>3 else 1000.0
    for c in chs:
        r=band(p,freq=fq,ch=c)
        if r is None:
            print("ch%-3d NO STEADY WINDOW (glitched capture) -- retake"%c); continue
        print("ch%-3d f=%8.1f tone %8.2f dBFS  rms %8.2f  noise %8.2f  T/N %7.2f  thd %7.2f  peak %7.2f  (%.0fs)"
              %(c,r['f'],r['tone'],r['rms'],r['noise'],r['snr'],r['thd'],r['peak'],r['secs']))
