# Installing libreac + reac-pw — the 2026-08-23 protocol alignment

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
struct's fields, and turned `reac_ctrl_build_name_frame` /
`_extra_frame` into `reac_ctrl_build_identity_first` / `_last`. reac-pw is
updated to match: it classifies by opcode and DT1 tag, never by frame length.

**The deployed `/usr/bin/reac-pw` cannot run against the new libreac.** Measured,
not assumed — the installed binary imports two symbols the new library no longer
exports:

    $ nm -D --undefined-only /usr/bin/reac-pw | grep _frame
                     U reac_ctrl_build_extra_frame
                     U reac_ctrl_build_name_frame
    $ LD_LIBRARY_PATH=<new> ./reac-pw-copy --help
    symbol lookup error: undefined symbol: reac_ctrl_build_name_frame

So it fails LOUDLY at load rather than misbehaving quietly, which is the good
case — but it means **libreac must not be installed on its own and left there.**
Install both, in the order below, before anything restarts.

A master already running keeps the old shared object mapped and is unaffected
until it is restarted. That is what makes step 9 a separate, deliberate decision.

## 1. THE VERSION DID NOT MOVE — read this before `dnf`/`rpm`

libreac broke API and is **still 0.6.0**, soname still `libreac.so.0`. Two
consequences:

* reac-pw's `dependency('libreac', version : '>=0.6.0')` floor accepts the OLD
  library and the NEW one alike. It cannot protect this upgrade. Nothing in the
  estate can tell the two apart by version.
* The RPM has the same NEVRA as the installed one, so `rpm -U` is a no-op.
  Use `--reinstall`, or bump `Release:` in `packaging/libreac.spec` first.

**The check that does work is the symbol table**, because only the new library
has these:

    nm -D --defined-only /usr/lib64/libreac.so.0 | grep identity_first

Empty output = the OLD library is still installed. This is the "ask for
something only the new build can answer" test; do not verify this upgrade by
version string, by `dnf list`, or by the daemon looking healthy.

## 2. Pre-flight — record what is running (unprivileged)

    pgrep -a -f '^/usr/bin/reac-pw'

Two masters were running when this sheet was written, both already explicit
about the rate:

    /usr/bin/reac-pw --live enp128s20f0u6 --tx enp128s20f0u6 --mixer m200 --rate 48000
    /usr/bin/reac-pw --live enp131s0 --tx enp131s0 --name s1608 --mixer m5000 --rate 48000 \
        --headamp 32:sens:32 --headamp 32:phantom:1 ... (16 channels, 32..47)

Capture the full command line of each before touching anything — step 9 needs
it verbatim.

    getcap /usr/bin/reac-pw
    nm -D --defined-only /usr/lib64/libreac.so.0 | grep -c identity_first

Expected now: `cap_net_admin,cap_net_raw,cap_sys_nice=ep` and `0`.

## 3. Build both, unprivileged, and prove the suites are green

    cd ~/Devel/audio/libreac
    make clean
    make test

    cd ~/Devel/audio/reac-pw
    meson setup build-rel --buildtype=release
    ninja -C build-rel
    meson test -C build-rel

`meson test` must read **35 ok, 1 skip**. The skip is `reac_pacer`, which exits
77 without `CAP_NET_RAW`; note that it still runs its earlier assertions before
skipping, so a red there is real.

`make corpus` in libreac is the capture-corpus gate. **It is currently RED for a
reason that is not libreac** — see §12.

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

## 7. Install libreac (root)

Either the RPM route — after bumping `Release:` in `packaging/libreac.spec`, or
with `--reinstall`, per §1:

    cd ~/Devel/audio/libreac
    ./packaging/build-rpm.sh
    dnf -y reinstall ~/rpmbuild/RPMS/x86_64/libreac-0.6.0-*.rpm ~/rpmbuild/RPMS/x86_64/libreac-devel-0.6.0-*.rpm

or, if installing the built object directly, place it and refresh the cache as
two commands:

    install -m0755 ~/Devel/audio/libreac/libreac.so.0.6.0 /usr/lib64/libreac.so.0.6.0

    ldconfig

Then confirm the new library is the one in place:

    nm -D --defined-only /usr/lib64/libreac.so.0 | grep identity_first

Must print a `T reac_ctrl_build_identity_first` line. If it prints nothing, stop:
the old library is still there and step 8 would install a binary that cannot load.

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

    ldd /usr/bin/reac-pw | grep libreac
    nm -D --undefined-only /usr/bin/reac-pw | grep identity_

The second must list `reac_ctrl_build_identity_first` and `_last`. If it lists
`reac_ctrl_build_name_frame` instead, the OLD binary is still installed.

## 10. Restarting the masters — the operator's call, not this sheet's

Two masters are driving real stageboxes. Nothing above restarts them; the
running processes hold the old library mapped and keep working until they are
stopped. When you choose to restart:

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

The pair reverts as a pair, in the reverse order — the library last, because a
new reac-pw against an old libreac fails to load exactly as the old one does
against the new.

    install -m0755 /root/reac-pw.backup-2026-08-23 /usr/bin/reac-pw

    setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep /usr/bin/reac-pw

    getcap /usr/bin/reac-pw

    install -m0755 /root/libreac.so.0.6.0.backup-2026-08-23 /usr/lib64/libreac.so.0.6.0

    ldconfig

    nm -D --defined-only /usr/lib64/libreac.so.0 | grep -c identity_first

The last must print `0` — the old library is back. Then restart the masters with
their §2 command lines.

## 12. Known-open, before you start

* **libreac's `make corpus` gate is RED, and not because of libreac.** The
  committed baseline was recorded against a different state of the capture
  corpus: 70 of 85 lines move, and every one of them moves ONLY in
  `records=` / `reac=` / `trunc=` / `filler=`. Strip those four fields and the
  file is byte-identical to the baseline — every classification, checksum, port
  and declaration count holds. `~/Devel/audio/reac-captures` also has 91 dirty
  paths and pcaps rewritten the same evening. The gate goes green again by
  re-recording the baseline once the corpus stops moving, in its own commit; do
  not re-record it as part of this install.
* **The version floor cannot see this change** (§1). Until libreac's version
  moves, the symbol-table check is the only mechanical guard.
