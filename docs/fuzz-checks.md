# The fuzz checks

Each property the fuzz targets in `fuzz/` check is an `abort()` in a `check_` function of its
harness. This file reads every one of them against two questions:

- **Does the check prove what its comment says, in both directions?** A check that tests a
  length for a value, runs only for some inputs, reads one element for a claim about each, or
  tests one direction of a two-way claim is weak. For each check we looked for a one-line change
  to reac-pw that keeps the check true on every input and makes the claim false.
- **Where does the expected value come from?** It must be worked out by the harness from the
  input and the written rule (the header, the protocol, the kernel's uapi), never taken from the
  code under test. `fuzz/pod.h` (the SPA POD record), `fuzz/netlink.h` (the fake rtnetlink
  kernel) and `fuzz/catalogue.c` (libreac's box-model rows, linked in place of libreac) are the
  harness's own and call no reac-pw code. Keys, role values and answer words that are libreac's
  published vocabulary (`<reac/reac_cfg.h>`) are read from that header directly; reac-pw's own
  aliases of them are never used by a harness.

How to read the tables: **Check** is the `abort()` line. **Killed by** lists the one-line
mutants recorded in the check's comment (`file:line on seed`) and the patches of
`fuzz/sabotage/`; `make -C fuzz fuzz-records` applies each one alone and shows that its seed
aborts at that very check, and `make -C fuzz fuzz-sabotage` does the same for the patches. The
stops marked `not a property` are allocation failures of the harness itself and are not listed.

Totals: 43 property checks, 124 mutant records and 8 breakage patches, all met; 43 OK, of which 5
were weak before this review and are now fixed. None takes its expected value from reac-pw.

## fuzz_pin: the `--box` pin

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_pin.c:41` | The pin is accepted exactly when the text before the first `:` is a whole model token. | The `--box` grammar (src/reac_box_pin.h:42-54), the harness splitting the spec itself; tokens from libreac's box table (fuzz/catalogue.c), looked up by the harness's own whole-token compare. | reac_box_pin.c:33 on prefix-s16; reac_box_pin.c:20 on s1608-left | OK |
| `fuzz_pin.c:52` | An accepted pin names the row of its own token. | The harness's own lookup in the same table. | reac_box_pin.c:35 on s0808 | OK |
| `fuzz_pin.c:68` | The label is the text after the first `:` (pointer into the spec), else the display name. | The grammar; the harness points at `spec + tok + 1` itself. | reac_box_pin.c:42 on s1608-left; reac_box_pin.c:44 on s1608-empty-label; reac_box_pin.c:45 on s0808; patch pin-label.patch | OK |

## fuzz_qdisc: the ETF catch-up budget

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_qdisc.c:89` | The ETF catch-up budget is (lead - 300 us) x fps / 1e6, rounded down, clamped to 1..1000000, and 1 for a rate <= 0. | src/reac_qdisc.h:108-129, with the qdisc delta of 300000 ns written in the harness from libreac's reac_etf_qdisc.h. | reac_qdisc.c:178 on lead-2500-at-8000; reac_qdisc.c:183 on lead-inside-delta; reac_qdisc.c:175 on negative-rate; reac_qdisc.c:185 on lead-max-at-max-rate; reac_qdisc.c:178 on lead-past-16-bits; patch qdisc-slots.patch | OK (was WEAK: lead read as 16 bits only; see Findings) |

## fuzz_rate: the `reac.cfg.rate` write

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_rate.c:140` | A Props write without `reac.cfg.rate` answers 0 and leaves the caller's rate unwritten. | fuzz/pod.h's record of the pod it wrote, walked by the pair rule; the key from libreac's published reac_cfg.h. | reac_rate_cfg.c:124 on params-not-a-struct; reac_rate_cfg.c:142 on near-keys | OK |
| `fuzz_rate.c:155` | An Int, or a Float whose nearest integer fits an int, answers 1 and that rate (the first rate key). | The pod record; the nearest integer worked out in fuzz/pod.h from the float's integer part and fraction, not by reac_float_near.h. | reac_rate_cfg.c:103 on rate-48000; reac_float_near.h:31 on rate-float-negative; reac_rate_cfg.c:140 on rate-after-other-key; patch rate-rate.patch | OK |
| `fuzz_rate.c:172` | Any other value, or a pod that is not a Props object, answers -1 and leaves the rate unwritten. | The pod record and the SPA accessor rules written in fuzz/pod.h. | reac_float_near.h:27 on rate-float-huge; reac_rate_cfg.c:112 on rate-bool; reac_rate_cfg.c:117 on not-props-rate | OK |
| `fuzz_rate.c:187` | The decision refuses a slave, then a rate off {44100, 48000, 96000}, then one outside the mask, else accepts. | src/reac_rate_cfg.h:59-60, 76-81; the closed list and its bits written in the harness. | reac_rate_cfg.c:87 on rate-not-drivable; reac_rate_cfg.c:83 on slave-asks-48000; reac_rate_cfg.c:26 on rate-44100 | OK |
| `fuzz_rate.c:199` | Each refusal publishes its own code string. | The code strings written in the harness (`not_closed`, `not_drivable`, `role_slave`, `malformed`, `none`), from libreac reac_cfg.h. | reac_rate_cfg.c:21 on rate-not-drivable; reac_rate_cfg.c:21 on slave-asks-48000 | OK |
| `fuzz_rate.c:214` | A rate's bit, its membership of the closed list and the best drivable rate follow the closed list. | The harness's own bit table (44100 bit 0) and the mask read highest first. | reac_rate_cfg.c:35 on rate-96000; reac_rate_cfg.c:43 on rate-not-drivable | OK |
| `fuzz_rate.c:228` | The drivable list is the drivable rates, ascending, comma separated, with snprintf length and truncation. | The harness writes the expected text digit by digit. | reac_rate_cfg.c:60 on rate-not-drivable; reac_rate_cfg.c:70 on rate-48000 | OK |

## fuzz_role: the `reac.cfg.role` write

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_role.c:75` | A Props write without `reac.cfg.role` answers 0 and leaves the role unwritten. | The pod record; the key from libreac's published reac_cfg.h (reac-pw's own aliases are not used). | reac_role_cfg.c:72 on params-not-a-struct; reac_role_cfg.c:90 on near-role-keys | OK |
| `fuzz_role.c:88` | 0 asks for the master and 1 for the slave, as an Int or a Float whose nearest integer it is. | The pod record and fuzz/pod.h's nearest integer; 0 and 1 from libreac's published reac_cfg.h. | reac_role_cfg.c:98 on role-master; reac_float_near.h:26 on role-float-below-half; patch role-role.patch | OK |
| `fuzz_role.c:104` | Any other value, no number, or not a Props object, answers -1 and leaves the role unwritten. | The pod record. | reac_role_cfg.c:100 on role-two; reac_float_near.h:31 on role-float-negative; reac_role_cfg.c:65 on not-props-role | OK |
| `fuzz_role.c:118` | The running role asserted again is no change and `applied`; the other role is a change and pending. | src/reac_role_cfg.h:80-90; the answer words from libreac's published reac_cfg.h. | reac_role_cfg.c:24 on role-master; reac_role_cfg.c:36 on role-master | OK |
| `fuzz_role.c:130` | The refusal codes are `none` and `malformed`. | Literal strings in the harness. | reac_role_cfg.c:19 on role-two; reac_role_cfg.c:19 on role-master | OK |

## fuzz_headamp: the head-amp cells

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_headamp.c:191` | The reader writes no cell past the capacity it is given. | A guard cell the harness fills with 0xa5. | reac_headamp_prop.c:169 on cap-one-two-cells | OK |
| `fuzz_headamp.c:204` | The return is -1 or 0..capacity, and the result says the same count with at least as many keys. | The capacity the harness chose; the exact key count is fuzz_headamp.c:270. | reac_headamp_prop.c:177 on cells-and-refusals; reac_headamp_prop.c:174 on cap-one-two-cells | OK |
| `fuzz_headamp.c:218` | Every cell handed on is a channel below 0x30, a param it has, and a value that param takes. | facts/reac_facts.h (0x30 channels, sens max 0x37) and the param numbers, written in the harness. | reac_headamp_prop.c:80 on channel-48; reac_headamp_prop.c:94 on phantom-two | OK |
| `fuzz_headamp.c:236` | -1 exactly for a NULL or non-Props pod, with an empty result; no params Struct is 0. | The pod record. | reac_headamp_prop.c:117 on not-props; reac_headamp_prop.c:120 on cells-and-refusals; reac_headamp_prop.c:127 on params-not-a-struct | OK |
| `fuzz_headamp.c:258` | The cells are exactly the well-formed keys, in order, with the channel, param and value they spell, up to the capacity. | The harness's reading of the key grammar (strtol's rules written out, the three param names) and of the value pod. | reac_headamp_prop.c:173 on cells-and-refusals; reac_headamp_prop.c:96 on sens-edges; reac_float_near.h:27 on sens-edges; reac_headamp_prop.c:74 on hex-channel; reac_headamp_prop.c:31 on cells-and-refusals; reac_headamp_prop.c:35 on phantom-256; patch headamp-cells.patch | OK |
| `fuzz_headamp.c:270` | Every key under the prefix the walk reached is counted, refused ones too. | The same reading. | reac_headamp_prop.c:153 on refused-only | OK |
| `fuzz_headamp.c:284` | The refusal reported is the first one, NONE only when every key stood. | The same reading. | reac_headamp_prop.c:164 on refusal-order; reac_headamp_prop.c:79 on refusal-order | OK |
| `fuzz_headamp.c:299` | The plain parse returns the count and writes the cells the grammar gives. | The harness's expectation (fuzz_headamp.c expect_of), no longer the other entry point. | reac_headamp_prop.c:105 on cap-one-two-cells; reac_headamp_prop.c:173 on sens-edges; patch headamp-plain.patch | OK (was WEAK: compared two entry points that share their code; see Findings) |

## fuzz_segconf: reac-pw.conf and its drop-ins

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_segconf.c:428` | The segments are the names the sections opened, once each, in order; past 32 is overflow; each parse returns the count. | The harness's line-by-line reading of the file grammar (src/reac_segconf.h:4-37, 46-115). | reac_segconf.c:107 on names-differ-in-case; reac_segconf.c:110 on thirty-three-segments; reac_segconf.c:177 on keyword-in-capitals; reac_segconf.c:178 on tab-after-keyword; reac_segconf.c:168 on text-after-header; reac_segconf.c:295 on operator-file; reac_segconf.c:331 on operator-file | OK |
| `fuzz_segconf.c:454` | A segment's role is the last one pinned, any case, unquoted; a box with no model falls back to no pin. | The same reading, with the cross-key rules applied by the harness. | reac_segconf.c:237 on role-in-capitals; reac_segconf.c:244 on operator-file; reac_segconf.c:321 on box-without-model; reac_segconf.c:307 on operator-file; reac_segconf.c:223 on operator-file; reac_segconf.c:146 on long-header-line; reac_segconf.c:190 on name-sixteen; reac_segconf.c:255 on model-in-capitals; patch segconf-role.patch | OK |
| `fuzz_segconf.c:467` | A model is a token of the box table, folded to lower case, and only on a box. | The same reading; tokens from libreac's table (fuzz/catalogue.c). | reac_segconf.c:308 on model-on-a-tap; reac_segconf.c:268 on model-in-capitals | OK |
| `fuzz_segconf.c:482` | `ignore` is the last yes/true/1 or no/false/0. | The same reading. | reac_segconf.c:90 on ignore-words; reac_segconf.c:278 on ignore-words; reac_segconf.c:530 on ignore-words | OK |
| `fuzz_segconf.c:499` | Each role and ignore answer names the file that gave it, or nothing. | The labels the harness handed to each parse. | reac_segconf.c:246 on dropin-overrides; reac_segconf.c:280 on dropin-overrides; reac_segconf.c:122 on dropin-overrides | OK |
| `fuzz_segconf.c:515` | A `knock` line is refused by name, with the reason `knock takes no plan`. | The documented refusal (reac-pw#61), as a literal. | reac_segconf.c:286 on knock-plans; reac_segconf.c:286 on knock-long-value; reac_segconf.c:286 on knock-plans | OK (was WEAK: checked the reason only, not the key; see Findings) |
| `fuzz_segconf.c:530` | Every unusable line is counted, and the first 8 kept. | The same reading, counting each refusal. | reac_segconf.c:32 on refusals-many; reac_segconf.c:207 on header-twice-in-one-file; reac_segconf.c:33 on refusals-many | OK |
| `fuzz_segconf.c:543` | A kept refusal starts with `<file>:<line>: `, or `<file>: ` for a cross-key rule. | The labels and line numbers the harness counted. | reac_segconf.c:39 on refusals-many; reac_segconf.c:38 on dropin-refusal | OK |
| `fuzz_segconf.c:561` | A name no section opened finds nothing: longer, shorter, in another case, or empty. | The harness's own list of opened names. | reac_segconf.c:522 on absent-probes; reac_segconf.c:522 on absent-probes; reac_segconf.c:522 on absent-probes | OK (was WEAK: probed only longer names; see Findings) |
| `fuzz_segconf.c:591` | The declared VLANs are the non-ignored `<parent>.<vid>` segments, vid 1..4094, each once, within the table and nothing past it. | src/reac_declared_vlan.h:26-38, the split and character rules written in the harness; a guard entry. | reac_declared_vlan.c:46 on vlans; reac_declared_vlan.c:43 on vlans; reac_declared_vlan.c:71 on vlans; reac_declared_vlan.c:73 on vlans-full; reac_segconf.c:568 on vlans; reac_declared_vlan.c:58 on vlans | OK |

## fuzz_link: the interface alias and the served mark

| Check | Property | Expected value from | Killed by | Verdict |
|---|---|---|---|---|
| `fuzz_link.c:496` | Every socket opened is closed, and no other descriptor is used. | The fake kernel's own bookkeeping (fuzz/netlink.h). | reac_link_alias.c:91 on read-served | OK |
| `fuzz_link.c:519` | Each operation sends exactly the requests it owes, in order, byte for byte, all of them. | Requests built by the harness from linux/rtnetlink.h and linux/netlink.h numbers. | reac_link_alias.c:138 on mark-plain-nic; reac_link_alias.c:147 on mark-plain-nic; reac_link_alias.c:117 on read-served; reac_served_mark.c:102 on unmark-after-host-renamed; reac_served_mark.c:64 on mark-plain-nic; reac_served_mark.c:98 on unmark-forty | OK (was WEAK: only the first 16 requests were seen; see Findings) |
| `fuzz_link.c:532` | A read or write that cannot be made (NULL or empty name, alias >= 256, no index) opens no socket. | The harness's interface table and IFALIASZ. | reac_link_alias.c:129 on write-no-index; reac_link_alias.c:126 on write-alias-256 | OK |
| `fuzz_link.c:555` | A read answers the reply's code (-EIO for a short error); on 0 the last INFO_KIND and IFLA_IFALIAS, cut at NUL and buffer. | The reply bytes the fake kernel handed out, walked by the netlink grammar in fuzz/netlink.h. | reac_link_alias.c:82 on read-vlan-with-data; reac_link_alias.c:77 on read-served; reac_link_alias.c:66 on read-error-short; reac_link_alias.c:61 on read-unpadded-last; reac_link_alias.c:70 on read-after-other; patch link-read.patch | OK |
| `fuzz_link.c:568` | A text written ends in a NUL inside its buffer and the byte after is untouched. | A guard byte the harness set. | reac_link_alias.c:32 on read-alias-fills-buffer; reac_link_alias.c:101 on read-plain-nic | OK |
| `fuzz_link.c:579` | A write answers the reply's code, and -ENAMETOOLONG for an alias of 256 bytes or more. | The reply bytes; IFALIASZ from linux/if.h. | reac_link_alias.c:67 on write-refused | OK |
| `fuzz_link.c:595` | An alias starting `reac-pw:` is ours, another alias is the host's, else a `vlan` kind is left, else marked. | src/reac_served_mark.h:23-28 and packaging/reac-pw-nm-unmanaged.sh, as literals. | reac_served_mark.c:29 on decide-all; reac_served_mark.c:33 on decide-all; reac_served_mark.c:28 on decide-all | OK |
| `fuzz_link.c:608` | A mark answers its verdict, or the error that stopped it, and -EINVAL with no marks or no name. | The harness's model of the mark (read, decide, write). | reac_served_mark.c:71 on mark-write-refused; reac_served_mark.c:91 on mark-minted-vlan | OK |
| `fuzz_link.c:625` | The marks kept are the interfaces this daemon marked or found marked by its own run, once each, at most 32; unmark clears them. | The same model. | reac_served_mark.c:78 on mark-minted-vlan; reac_served_mark.c:41 on mark-twice; reac_served_mark.c:106 on unmark-after-host-renamed; reac_served_mark.c:43 on mark-forty | OK |

## Findings

Five checks were weak. Each was fixed in the harness, and each fix carries a mutant record that
the old check let through and the new one catches.

1. `fuzz_qdisc.c:89` read the lead as 16 bits, while `reac_qdisc_etf_catchup_slots` takes any
   `unsigned`. A change such as `unsigned usable` -> `unsigned short usable`
   (src/reac_qdisc.c:178) broke every lead above 65535 us and no check could see it. The lead is
   now 32 bits; seed `lead-past-16-bits` catches that mutant, and `lead-max-at-max-rate` now
   uses the largest lead.
2. `fuzz_headamp.c:299` compared `reac_headamp_prop_parse` with
   `reac_headamp_prop_parse_result`. The first calls the second, so a bug in the shared code
   broke both sides alike. The plain parse is now compared with the harness's own expectation,
   and its cells are checked for range and overrun like the other parse's.
3. `fuzz_segconf.c:515` looked for `takes no plan` anywhere in the refusal, while its comment
   says the line is refused by name. It now looks for `knock takes no plan`; the mutant
   `knock takes` -> `the key takes` (src/reac_segconf.c:286) is caught on `knock-plans`.
4. `fuzz_segconf.c:561` probed only names one byte longer than a segment and one unknown name.
   `strcmp` -> `strncmp(..., strlen(name))` or `strcasecmp` in `reac_segconf_find`
   (src/reac_segconf.c:522) answered for a prefix or another case and stayed unseen. The probes
   now add each name cut by one byte, each name with its case turned, and the empty name; both
   mutants are caught on `absent-probes`.
5. `fuzz_link.c:519` compared the requests sent with those owed, but the fake kernel and the
   model both stopped recording at 16, so an unmark of the 32 kept marks could drop every
   request past the 16th. Both now keep 80 and count past that; the mutant `i < m->n;` ->
   `i < m->n && i < 16;` (src/reac_served_mark.c:98) is caught on the new seed `unmark-forty`.
