# data/ — provenance

## `m200i-scene-8904.bin`

**Recovered from a capture. Not synthesised, and not our console's state.**

- **What it is:** the 8904-byte scene body a Roland M-200i pushes to a stagebox
  after link-up — one op-0101 header, 341 op-0100 chunks of 26 bytes, one
  op-0102 final of 14. `24 + 341*26 + 14 = 8904 = 0x22c8`.
- **Where it came from:** `tools/recover-scene.py` over
  `m200i-s1608-48k-mirror__real-m200-s1608-coldboot-2026-07-11.pcap`
  in the private `reac-captures` repo. The tool self-checks on the 8904 total and
  refuses a decimated capture rather than yielding a plausible short body.
- **Why it is here:** it is a **working example** of a legal scene, and reac-pw
  sends it so a box will complete its reassembly and run the state-4 commit that
  promotes head-amp. Our own MAC is substituted at `+0x340` before it goes out;
  nothing else is changed.

### It is an example, not the right end state

The box validates only three four-byte tags in this body — `"1234"` at `+0x000`,
`"SYSP"` at `+0x368`, `"SCEN"` at `+0x37c` — but it **reads far more of it than
it validates**. Measured on a real S-1608: a synthetic body of zeros plus those
three tags passes the commit and still leaves the box reporting `model=unknown`
with **zero** capture ports, where this recovered body brings it up as `s1608`
with 16. So this file also carries an M-200i's idea of what the desk is, and a
console that generates its own scene is the correct behaviour independently of
what may be published.

Generating a body means reproducing the structure the box reads for its own
configuration, not merely satisfying the three compares. That is follow-on work.
