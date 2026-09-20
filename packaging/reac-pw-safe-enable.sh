#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw-safe-enable.sh -- the ONE place that decides whether a plain RPM install may
# auto-enable reac-pw.service for a real user, and for whom. Called from %posttrans
# (packaging/reac-pw.spec) after the whole dnf transaction settles -- never %post: mid-
# transaction, siblings in the same install are not necessarily in place yet.
#
# THE RULE, whole: enable for a user ONLY when there is EXACTLY ONE real, already-open,
# INTERACTIVE human login session on the host (systemd-logind class "user", never uid 0).
# Zero such sessions (an unattended install, or an image with nobody logged in yet) or
# more than one (ambiguous -- an admin's own desktop session alongside a second console
# user, say) leave the unit exactly as an untouched install: the operator runs
# `systemctl --user enable --now reac-pw` by hand, same as today (README.md).
#
# WHY "class user" AND NOT ANY SESSION WITH A MATCHING UID. Measured on a real desk
# (2026-09-20): `loginctl list-sessions` routinely carries several uid-0 rows of class
# `manager-early`/`user-early` that are systemd-internal placeholders, not a human
# logged in as root -- and a real user who has `loginctl enable-linger` set (openmixer's
# own firstboot.sh does this) ALSO shows a class-`manager` row with no seat/tty that
# exists whether or not anyone is actually sitting at the console. Only class `user`
# is systemd-logind's own marker for an actual interactive login. Filtering on uid alone,
# or counting any session row, both miscount against this real data.
#
# THIS is the ONE mechanism `systemctl --machine=<user>@.host --user enable --now`
# cannot reach a session that does not exist yet, or the wrong one of several -- which
# is the whole lesson of the 1.0.19 incident (packaging/reac-pw.spec %post, README.md):
# `%post` running as root during `dnf install` has no reliable idea who "the console
# user" is, and guessing wrong is exactly how a second daemon won the segment lock.
#
# Split out of the spec file so this selection logic is testable without a real rpm
# transaction or a real login session: tests/safe-enable-selects-one-user.sh mocks
# `loginctl`/`systemctl` via REACPW_LOGINCTL/REACPW_SYSTEMCTL and asserts the decision.

set -eu

loginctl_bin=${REACPW_LOGINCTL:-loginctl}
systemctl_bin=${REACPW_SYSTEMCTL:-systemctl}

# Columns (systemd 254-259, verified against a real host 2026-09-20):
#   SESSION UID USER SEAT LEADER CLASS TTY IDLE SINCE
# $2 = UID, $3 = USER, $6 = CLASS. Real, interactive, non-root sessions only.
users=$("$loginctl_bin" list-sessions --no-legend 2>/dev/null | \
    awk '$2 != "0" && $6 == "user" { print $3 }' | sort -u)

count=$(printf '%s\n' "$users" | grep -c . || true)

if [ "$count" -ne 1 ]; then
    echo "reac-pw-safe-enable: $count real interactive login session(s), not exactly 1 -- not auto-enabling (run 'systemctl --user enable --now reac-pw' as the console user)" >&2
    exit 0
fi

user=$users

echo "reac-pw-safe-enable: exactly one real interactive session ($user) -- enabling reac-pw.service for it" >&2
if ! "$systemctl_bin" --machine="${user}@.host" --user enable --now reac-pw.service >/dev/null 2>&1; then
    echo "reac-pw-safe-enable: enable --now failed for $user -- the manual instructions still apply" >&2
fi
