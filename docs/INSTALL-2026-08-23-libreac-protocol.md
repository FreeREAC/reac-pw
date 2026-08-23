# Installing libreac 0.7.0 + reac-pw 0.3.0 — the 2026-08-23 protocol alignment

**Run every numbered command on its own. Nothing below is chained with `&&`,
and the backup and its verification are deliberately two separate commands: a
`mv`/`cp` that failed inside an `&&` chain once took an irreplaceable directory
with it, because the `rm` behind it ran anyway.**

`main` of both repos is prepared but NOT pushed and NOT installed. Steps 4-8
need root and are the operator's to run. Steps 9-10 restart live masters and are
the operator's decision, not a step in a script.

## 0. What this is, and why the two go in together

libreac now reads the REAC control block's header as the four fields the box's
own builders lay down — link, segment, length, opcode — instead of a two-byte
"opcode" over the first two. That removed `REAC_CTRL_PROBE`, renamed the parsed
struct's fields, and turned `reac_ctrl_build_name_frame` / `_extra_frame` into
`reac_ctrl_build_identity_first` / `_last`. reac-pw is updated to match: it
classifies by opcode and DT1 tag, never by frame length.

It also carries the head-amp base correction: a box's base is the chassis strap
it ANNOUNCES (config announce `01 03 00 10` at `block[7]`, times `0x10`), not a
value derived from its input width. The per-width table is deleted from libreac
and from both places reac-pw was inferring it.

**The deployed `/usr/bin/reac-pw` cannot run against the new libreac.** Measured,
not assumed — the installed binary imports two symbols the new library no longer
exports, and the two now disagree about the soname as well:

    $ objdump -p /usr/bin/reac-pw | grep NEEDED | grep reac
      NEEDED               libreac.so.0
    $ nm -D --undefined-only /usr/bin/reac-pw | grep _frame
                     U reac_ctrl_build_extra_frame
                     U reac_ctrl_build_name_frame

    $ objdump -p <new build>/reac-pw | grep NEEDED | grep reac
      NEEDED               libreac.so.1
    $ nm -D --undefined-only <new build>/reac-pw | grep identity
                     U reac_ctrl_build_identity_first

So: install both, in the order below, before anything restarts.

A master already running keeps its old shared object mapped and is unaffected
until it is restarted — the mapping survives even if the file is replaced or
unlinked underneath it. That is what makes step 10 a separate, deliberate
decision, and it is the reason nothing in steps 4-9 touches a running process.

## 1. THE VERSION MOVED THIS TIME — and so did the soname

The previous cut of this change shipped as libreac **0.6.0 with soname
`libreac.so.0` unchanged**, after removing two public functions. Every guard in
the estate was inert at once: reac-pw's `>= 0.6.0` floor accepted the library
with and without the symbols, the identical NEVRA made `rpm -U` a no-op, and the
installed binary loaded the new `.so.0` and died on `undefined symbol`.

What is different now:

* **libreac is 0.7.0, soname `libreac.so.1`.** The version refuses a BUILD
  against the wrong headers; the soname refuses a RUN against the wrong shared
  object. They fail at different moments, which is why a removed symbol needs
  both.
* **reac-pw is 0.3.0** with its floor raised to `>= 0.7.0`. The floor was
  measured in all four combinations before this sheet was written:

      old floor >=0.6.0 x libreac 0.6.0  -> ACCEPTED
      old floor >=0.6.0 x libreac 0.7.0  -> ACCEPTED     <- the defect
      new floor >=0.7.0 x libreac 0.6.0  -> REFUSED at configure:
          "Dependency libreac ... found: NO. Found 0.6.0 but need: '>=0.7.0'"
      new floor >=0.7.0 x libreac 0.7.0  -> ACCEPTED

* **The NEVRA changes**, so `rpm -U` is a real upgrade rather than a no-op.

**What the soname bump buys at install time.** rpm generates the runtime
dependency from the soname, and the installed pair currently reads:

    $ rpm -q --requires reac-pw | grep libreac
    libreac.so.0()(64bit)
    $ rpm -q --provides libreac | grep libreac.so
    libreac.so.0()(64bit)

libreac 0.7.0 provides `libreac.so.1()(64bit)` and nothing else. So upgrading
libreac **alone** now breaks a dependency dnf can see: the transaction is
refused, or it pulls reac-pw 0.3.0 in with it. Under soname 0 the same
transaction succeeded silently and left a binary that could not start.

**The check that proves which library is in place is still the symbol table**,
because it asks for something only the new build can answer:

    nm -D --defined-only /usr/lib64/libreac.so.1 | grep identity_first

Do not verify this upgrade by version string, by `dnf list`, or by the daemon
looking healthy.

## 2. Pre-flight — record what is running (unprivileged)

    pgrep -a -f '^/usr/bin/reac-pw'

Two masters were running when this sheet was written, both already explicit
about the rate:

    /usr/bin/reac-pw --live enp128s20f0u6 --tx enp128s20f0u6 --mixer m200 --rate 48000
    /usr/bin/reac-pw --live enp131s0 --tx enp131s0 --name s1608 --mixer m5000 --rate 48000 \
        --headamp 32:sens:32 --headamp 32:phantom:1 ... (16 channels, 32..47)

Capture the full command line of each before touching anything — step 10 needs
it verbatim.

    getcap /usr/bin/reac-pw

    ls -l /usr/lib64/libreac.so.*

Expected now: `cap_net_admin,cap_net_raw,cap_sys_nice=ep`, and a
`libreac.so.0 -> libreac.so.0.6.0` with **no `libreac.so.1` present**.

## 3. Build both, unprivileged, and prove the suites are green

    cd ~/Devel/audio/libreac
    make clean
    make test

`make test` ends with the source-shape conformance arm, which must print
`OK: head-amp base has one source — the announced strap`. It is a grep, not a
value test, on purpose: a per-width base table agrees with the wire on all three
chassis we own, so no test built from our own captures can catch its return.

    cd ~/Devel/audio/reac-pw
    meson setup build-rel --buildtype=release
    ninja -C build-rel
    meson test -C build-rel

`meson test` must read **35 ok, 1 skip**. The skip is `reac_pacer`, which exits
77 without `CAP_NET_RAW`; note that it still runs its earlier assertions before
skipping, so a red there is real.

libreac's capture-corpus gate, and its two self-tests — **read §12 first if
either behaves oddly**, there is a known in-flight capture:

    cd ~/Devel/audio/libreac
    make corpus
    tools/run-corpus.sh --self-test
    tools/run-corpus.sh --self-test-audio

## 4. Back up the binary. On its own. (root)

    cp -a /usr/bin/reac-pw /root/reac-pw.backup-2026-08-23

## 5. Verify the backup exists. Separate command. (root)

    ls -l /root/reac-pw.backup-2026-08-23

Do not continue unless that prints a file of the expected size (131168 bytes for
the 2026-08-22 build). No output, or an error, means step 4 failed and step 6
would destroy the only copy.

## 6. Back up the library, and verify that too. (root)

    cp -a /usr/lib64/libreac.so.0.6.0 /root/libreac.so.0.6.0.backup-2026-08-23

    ls -l /root/libreac.so.0.6.0.backup-2026-08-23

Expect 27960 bytes. Same rule as step 5: verify before anything overwrites.

## 7. Install libreac 0.7.0 (root)

The RPM route — the NEVRA moves now, so this is an ordinary upgrade. Because
reac-pw 0.2.0 requires `libreac.so.0`, dnf will refuse libreac on its own; give
it both packages in ONE transaction (build reac-pw's RPM in step 8 first if you
take this route):

    cd ~/Devel/audio/libreac
    ./packaging/build-rpm.sh

    dnf -y upgrade ~/rpmbuild/RPMS/x86_64/libreac-0.7.0-*.rpm \
                   ~/rpmbuild/RPMS/x86_64/libreac-devel-0.7.0-*.rpm \
                   ~/rpmbuild/RPMS/x86_64/reac-pw-0.3.0-*.rpm

Or, installing the built object directly — three commands, because the soname
symlink is a separate fact from the file:

    install -m0755 ~/Devel/audio/libreac/libreac.so.0.7.0 /usr/lib64/libreac.so.0.7.0

    ln -sf libreac.so.0.7.0 /usr/lib64/libreac.so.1

    ldconfig

The direct route leaves `libreac.so.0` in place, so the old binary stays
runnable until step 8 replaces it. The RPM route removes it; either is fine as
long as steps 7 and 8 are not left half-done.

Then confirm the new library is the one in place:

    nm -D --defined-only /usr/lib64/libreac.so.1 | grep identity_first

Must print a `T reac_ctrl_build_identity_first` line. If it prints nothing, or
`libreac.so.1` does not exist, stop: step 8 would install a binary that cannot
load.

## 8. Install reac-pw, then set its capabilities. TWO COMMANDS. (root)

    install -m0755 ~/Devel/audio/reac-pw/build-rel/reac-pw /usr/bin/reac-pw

    setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep /usr/bin/reac-pw

**`setcap` MUST come after `install`, every time.** Capabilities live on the
inode and `install` creates a new one, so an install silently drops them. Then
verify, as its own command:

    getcap /usr/bin/reac-pw

**`getcap` printing nothing is a hard failure — go back and run `setcap`.** Do
not proceed on a binary with no capabilities and do not judge it by whether it
starts: without them reac-pw can publish its nodes, log normally and look
completely healthy while syncing no box at all.

## 9. Verify the installed pair before restarting anything (root or user)

    objdump -p /usr/bin/reac-pw | grep NEEDED | grep reac

    nm -D --undefined-only /usr/bin/reac-pw | grep identity_

    ldd /usr/bin/reac-pw | grep libreac

The first must read `libreac.so.1`; the second must list
`reac_ctrl_build_identity_first` and `_last`. If it lists
`reac_ctrl_build_name_frame` instead, the OLD binary is still installed. The
third must resolve — a `not found` there means step 7 did not finish.

## 10. Restarting the masters — the operator's call, not this sheet's

Two masters are driving real stageboxes. **Nothing above restarts them**, and
they keep running on their mapped copy of the old library until they are
stopped. The restart is the operator's to run, at a moment of the operator's
choosing. When you choose to:

* snapshot routing first — route healing is unproven and a restart can drop
  links;
* bring each one back with its **`--rate 48000` explicit**. The master default is
  now **96000** and this rig runs 48k; omitting the flag changes the rate the
  whole segment locks to, because a master defines the pace.

        /usr/bin/reac-pw --live enp128s20f0u6 --tx enp128s20f0u6 --mixer m200 --rate 48000

        /usr/bin/reac-pw --live enp131s0 --tx enp131s0 --name s1608 --mixer m5000 --rate 48000 \
            --headamp 32:sens:32 --headamp 32:phantom:1 ...   # the rest verbatim from §2

* **rate matching ships OFF.** `REACPW_RATE_MATCH=1` is the opt-in and this
  upgrade does not change that; leave it unset unless you are deliberately
  testing it.
* **slot-debt catch-up is ON**, 4 slots by default. `REACPW_CATCHUP_MAX_SLOTS=-1`
  restores the pre-2026-08-23 behaviour if it ever needs excluding.

## 11. Revert

The pair reverts as a pair, in the reverse order — the binary first, the library
last, because a new reac-pw against an old libreac fails to load exactly as the
old one does against the new.

    install -m0755 /root/reac-pw.backup-2026-08-23 /usr/bin/reac-pw

    setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep /usr/bin/reac-pw

    getcap /usr/bin/reac-pw

If the RPM route removed it, restore the old library too; the direct route left
it in place and only the symlink needs retiring:

    install -m0755 /root/libreac.so.0.6.0.backup-2026-08-23 /usr/lib64/libreac.so.0.6.0

    ln -sf libreac.so.0.6.0 /usr/lib64/libreac.so.0

    rm -f /usr/lib64/libreac.so.1

    ldconfig

Then the post-revert checks, each on its own:

    nm -D --defined-only /usr/lib64/libreac.so.0 | grep -c identity_first

    objdump -p /usr/bin/reac-pw | grep NEEDED | grep reac

The first must print `0` and the second `libreac.so.0` — the old pair is back.
Then restart the masters with their §2 command lines.

## 12. Known-open, before you start

* **One capture in the corpus is being re-distilled right now, and it is NOT a
  libreac regression.** `make corpus` reports a single moved line,
  `m200-headamp-re/m200i-s0808-48k-mirror__ctl2.pcap`: the file is modified but
  uncommitted in `~/Devel/audio/reac-captures`, and it lost 5,987,703 records —
  exactly the drop in `grant=` and `L4.3.02=`, with all 22 other counters on that
  line and all 84 other captures byte-identical to the baseline. Verified by
  re-running the gate over the 84 stable captures: **identical to the baseline**.
* **That drift also disarms both self-tests, in opposite directions.** Each
  compares against the committed baseline rather than against a clean run of the
  same corpus, so while the corpus disagrees with the baseline:
  `--self-test` reports OK **whatever the sabotage did** (its pass condition,
  "output differs from baseline", is already met), and `--self-test-audio`
  reports `INCONCLUSIVE` and exits 1 because the control counts moved for a
  reason that has nothing to do with its sabotage. Both arms were re-run over
  the 84 stable captures and are genuinely live there: the control arm moved 82
  lines under sabotage, and the audio arm moved `dn=`/`up=` while every control
  count held. **Do not re-record the baseline to make the gate green** — the
  captures are mid-distillation and the baseline would bake in a moving target.
* **Do not judge this upgrade by version string alone.** The floor and the
  soname now work (§1), but the symbol-table check is what proves which bits are
  actually in `/usr/lib64`.
