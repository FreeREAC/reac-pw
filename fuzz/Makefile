# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE libFuzzer HARNESSES OF reac-pw's OWN READERS (fuzz/README.md). One harness per
# reader, each a plain C LLVMFuzzerTestOneInput built by fuzz/build.sh with clang into
# build/fuzz/; the meson build is not touched. Run from the top of the tree:
#
#   make -C fuzz fuzz                     every harness, with ASan and UBSan
#   make -C fuzz fuzz-run FUZZ_TIME=60    fuzz each from its seed corpus; red on any crash,
#                                         the input left in fuzz/findings/
#   make -C fuzz fuzz-sabotage            each patch of fuzz/sabotage/ must make its harness
#                                         abort, over the corpus, in the check it names
#   make -C fuzz fuzz-records             each check's record, alone, aborts at that check on
#                                         its seed; red on a check with no record and no
#                                         `not a property` line
#   make -C fuzz fuzz-coverage            the coverage witness: llvm-cov over the corpus, held
#                                         to fuzz/coverage-floor.txt and to every check
#   make -C fuzz fuzz-static              each harness linked fully static, over its corpus
#   make -C fuzz fuzz-corpus              write fuzz/corpus/ again from fuzz/gen-corpus.c;
#                                         fuzz-corpus-check: the tree's corpus is what it writes
ROOT          := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
FUZZ_CC       ?= clang
FUZZ_TIME     ?= 60
FUZZ_BUILD    := build/fuzz
HARNESSES     := segconf pin headamp rate role qdisc link
LLVM_COV      ?= llvm-cov
LLVM_PROFDATA ?= llvm-profdata
LLVM_READELF  ?= llvm-readelf
export FUZZ_CC

fuzz:
	cd $(ROOT) && for h in $(HARNESSES); do fuzz/build.sh san $$h $(FUZZ_BUILD)/fuzz_$$h || exit 1; done

fuzz-run: fuzz
	cd $(ROOT) && FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_TIME=$(FUZZ_TIME) fuzz/run.sh $(HARNESSES)

fuzz-sabotage:
	cd $(ROOT) && FUZZ_BUILD=$(FUZZ_BUILD) fuzz/sabotage.sh

fuzz-records:
	cd $(ROOT) && FUZZ_BUILD=$(FUZZ_BUILD) fuzz/records.sh $(HARNESSES)

fuzz-coverage:
	cd $(ROOT) && set -e; for h in $(HARNESSES); do \
	  fuzz/build.sh cov $$h $(FUZZ_BUILD)/cov/fuzz_$$h; \
	  rm -f $(FUZZ_BUILD)/cov/$$h.profraw; \
	  LLVM_PROFILE_FILE=$(FUZZ_BUILD)/cov/$$h.profraw $(FUZZ_BUILD)/cov/fuzz_$$h -runs=0 fuzz/corpus/$$h/ \
	      >$(FUZZ_BUILD)/cov/$$h.log 2>&1 || \
	    { cat $(FUZZ_BUILD)/cov/$$h.log; echo "fuzz-coverage: fuzz_$$h failed on its own corpus"; exit 1; }; \
	  $(LLVM_PROFDATA) merge -sparse $(FUZZ_BUILD)/cov/$$h.profraw -o $(FUZZ_BUILD)/cov/$$h.profdata; \
	done; \
	LLVM_COV=$(LLVM_COV) fuzz/coverage.sh $(FUZZ_BUILD)/cov $(HARNESSES)

# the build a verifier that brings its own driver takes: fuzz/standalone.c for the driver, no
# library beyond libc and libm, and every seed run through it
fuzz-static:
	cd $(ROOT) && set -e; \
	$(LLVM_READELF) -l $$(command -v $(FUZZ_CC)) | grep -qE 'INTERP|DYNAMIC' || \
	  { echo "fuzz-static: the test does not see the dynamic link of $(FUZZ_CC) itself"; exit 1; }; \
	for h in $(HARNESSES); do \
	  fuzz/build.sh static $$h $(FUZZ_BUILD)/static/fuzz_$$h; \
	  if $(LLVM_READELF) -l $(FUZZ_BUILD)/static/fuzz_$$h | grep -qE 'INTERP|DYNAMIC'; then \
	    echo "fuzz-static: fuzz_$$h is not linked static"; exit 1; fi; \
	  echo "fuzz_$$h, static: $$($(FUZZ_BUILD)/static/fuzz_$$h fuzz/corpus/$$h/*)"; \
	done

$(ROOT)/$(FUZZ_BUILD)/gen-corpus: $(ROOT)/fuzz/gen-corpus.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) -O1 -g -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -o $@ $<

fuzz-corpus: $(ROOT)/$(FUZZ_BUILD)/gen-corpus
	cd $(ROOT) && rm -rf fuzz/corpus && $(FUZZ_BUILD)/gen-corpus fuzz/corpus

fuzz-corpus-check: $(ROOT)/$(FUZZ_BUILD)/gen-corpus
	cd $(ROOT) && tmp=$$(mktemp -d) && $(FUZZ_BUILD)/gen-corpus $$tmp/corpus && \
	  diff -r $$tmp/corpus fuzz/corpus && rm -rf $$tmp && echo "fuzz-corpus-check: the seed corpus is current"

.PHONY: fuzz fuzz-run fuzz-sabotage fuzz-records fuzz-coverage fuzz-static fuzz-corpus \
        fuzz-corpus-check
