#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# packaging/reac-pw-safe-enable.sh ENABLES FOR EXACTLY ONE REAL INTERACTIVE USER, OR NOBODY.
#
# The rule under test is the whole safety argument of the %posttrans auto-enable: the unit
# is enabled only when there is exactly one class-`user`, non-root login session, and in
# every other case -- none, several, root-only, linger-only -- the script touches nothing.
# `loginctl` and `systemctl` are mocked through REACPW_LOGINCTL / REACPW_SYSTEMCTL, so this
# asserts the DECISION (which user, or none) and needs no rpm transaction and no session.
#
# The session tables are the real shape measured on a desk on 2026-09-20 (systemd 259):
# several uid-0 `manager-early`/`user-early` rows, and a lingering user's seatless
# class-`manager` row beside their class-`user` one. A filter that counted rows, or
# filtered on uid alone, gets arm A wrong -- which is why arm A is that table verbatim.
set -u

script=${1:?usage: safe-enable-selects-one-user.sh <path to reac-pw-safe-enable.sh>}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0

cat >"$work/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$CALLS"
EOF
chmod +x "$work/systemctl"

# run <arm> <expected systemctl call, or empty for "must not be called"> <<table
run() {
    arm=$1 want=$2
    table=$(cat)
    printf '#!/bin/sh\ncat <<"TABLE"\n%s\nTABLE\n' "$table" >"$work/loginctl"
    chmod +x "$work/loginctl"
    : >"$work/calls"
    CALLS="$work/calls" REACPW_LOGINCTL="$work/loginctl" REACPW_SYSTEMCTL="$work/systemctl" \
        sh "$script" 2>"$work/stderr"
    rc=$?
    got=$(cat "$work/calls")
    if [ "$rc" -ne 0 ]; then
        echo "FAIL $arm: exit $rc (a scriptlet helper must never fail the transaction)"; fail=1
    elif [ "$got" != "$want" ]; then
        echo "FAIL $arm: systemctl called with [$got], wanted [$want]"; fail=1
    else
        echo "ok   $arm"
    fi
}

# A -- the real desk table: one human (two rows: linger manager + tty login), root noise.
run "A one real user among root placeholders" \
    "--machine=pau@.host --user enable --now reac-pw.service" <<'EOF'
 1 1000 pau  -     2189  manager       -       no -
 3 1000 pau  seat0 6027  user          tty2    no -
 4    0 root -     61139 manager-early -       no -
c3    0 root -     61133 user-early    pts/133 no -
EOF

# B -- nobody logged in (unattended install / image build): touch nothing.
run "B no sessions at all" "" <<'EOF'
EOF

# C -- two real interactive users: ambiguous, never guess.
run "C two real users" "" <<'EOF'
 3 1000 pau   seat0 6027 user tty2  no -
 7 1001 guest seat0 7044 user tty3  no -
EOF

# D -- a lingering user with NO interactive login: a manager row is not a person at the desk.
run "D linger-only, no login" "" <<'EOF'
 1 1000 pau  -     2189  manager       -       no -
EOF

# E -- a real root login (class user, uid 0): never root, even alone.
run "E root interactive only" "" <<'EOF'
 9    0 root seat0 8100  user          tty1    no -
EOF

# F -- one real user plus a real root login: root does not make it ambiguous.
run "F one real user beside a root login" \
    "--machine=pau@.host --user enable --now reac-pw.service" <<'EOF'
 3 1000 pau  seat0 6027  user          tty2    no -
 9    0 root seat0 8100  user          tty1    no -
EOF

exit $fail
