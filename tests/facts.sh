# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Sourced by the shell tests: every REAC protocol number as FACT_<NAME>, read from the one
# declaration (facts/reac_facts.h, or $REACPW_FACTS_DIR) by tools/facts.py. Exported, so an
# `unshare ... bash -s` body inherits them. `--rate "$FACT_SAMPLE_RATE_96K"`, never 96000.
eval "$(python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/facts.py" env)" || {
	echo "FAIL: tools/facts.py could not read the protocol facts"; exit 1; }
