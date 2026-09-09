#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a signed dnf repository tree for the FreeREAC/openmixer RPM family
# (task #320). This is the hand-run publishing layer; the tag-triggered GitHub
# Actions job (#321) is expected to call exactly this script, not reimplement it.
#
#   packaging/publish-repo.sh --rpm-dir ../libreac/build/rpm --rpm-dir build/rpm
#   packaging/publish-repo.sh --build            # build all four in order first
#   packaging/publish-repo.sh --rpm-dir DIR --no-sign   # unsigned smoke run
#
# What it does, in order:
#   1. (optional) builds mod-host -> libreac -> reac-pw -> openmixer via each
#      project's own build entry point -- never a second copy of their build logic.
#   2. sorts every collected .rpm into <out>/rpm/fedora/<releasever>/<arch>/,
#      deriving releasever from the package's .fcNN dist tag and arch from the
#      package itself (src.rpm -> SRPMS).
#   3. signs each package with rpmsign (gpg-agent supplies the passphrase --
#      this script never reads, stores or passes one).
#   4. runs createrepo_c over every populated directory.
#   5. detach-signs repodata/repomd.xml so repo_gpgcheck=1 works.
#   6. drops the .repo file and the exported public key into the tree.
#
# Re-runnable: repeated runs over the same output tree replace packages of the
# same NEVRA, re-use createrepo_c --update, and refresh the repomd signature.
# It NEVER pushes anywhere -- publishing the emitted tree (GitHub Pages branch,
# object store, whatever) is a separate, deliberate step.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPLATE_DIR="$ROOT/packaging/repo"

# ---------------------------------------------------------------- defaults --
REPO_ID="freereac"
REPO_NAME="FreeREAC / openmixer"
BASE_URL="https://freereac.github.io/rpm"
KEY_ID="A14B3E1E1F69EBF4"
OUT="$ROOT/build/repo"
SIGN=1
BUILD=0
LIBREAC_DIR="${LIBREAC_DIR:-$ROOT/../libreac}"
REACPW_DIR="${REACPW_DIR:-$ROOT/../reac-pw}"
declare -a RPM_DIRS=()
declare -a RELEASEVERS=()
declare -a ARCHES=()
FALLBACK_RELEASEVER=""
# Private-target read credentials (task: private publishing, delivery-pipeline amendment
# 2026-09-08). Optional -- when both are set, the emitted .repo carries username=/password= so a
# Basic-Auth-gated private tree (e.g. an R2 bucket behind a Worker) reads the same way a public
# GitHub Pages tree does. Never logged, never echoed.
REPO_USERNAME=""
REPO_PASSWORD=""

die() { echo "FATAL: $*" >&2; exit 1; }
say() { echo "==> $*"; }

usage() {
  cat >&2 <<EOF
usage: $0 [options]

  --rpm-dir DIR        directory searched recursively for *.rpm (repeatable)
  --build              build mod-host -> libreac -> reac-pw -> openmixer first,
                       and take their output directories as --rpm-dir
  --out DIR            output tree root                (default: $OUT)
  --repo-id ID         dnf repo id / .repo basename    (default: $REPO_ID)
  --repo-name NAME     human repo name                 (default: $REPO_NAME)
  --base-url URL       public URL of the tree's rpm/ dir
                                                       (default: $BASE_URL)
  --key-id ID          GPG key to sign with            (default: $KEY_ID)
  --releasever V       only publish packages for this Fedora release
                       (repeatable; default: whatever the packages carry)
  --arch A             only publish this arch (repeatable; SRPMS counts as one)
  --fallback-releasever V
                       release to file packages that carry no .fcNN dist tag
                       under (default: the build host's own %fedora)
  --no-sign            skip rpmsign + repomd signing. Emits repo_gpgcheck=0 and
                       says so loudly. For smoke tests only -- never publish it.
  --repo-username U    HTTP Basic Auth username baked into the emitted .repo, for a
                       private target gated by Basic Auth (e.g. an R2 bucket behind a
                       Worker). Requires --repo-password too. Never logged.
  --repo-password P    HTTP Basic Auth password baked into the emitted .repo. Never
                       logged. Pass both or neither -- one without the other produces
                       a .repo dnf cannot authenticate with.
  -h, --help           this text
EOF
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --rpm-dir)   RPM_DIRS+=("$2"); shift 2 ;;
    --build)     BUILD=1; shift ;;
    --out)       OUT="$2"; shift 2 ;;
    --repo-id)   REPO_ID="$2"; shift 2 ;;
    --repo-name) REPO_NAME="$2"; shift 2 ;;
    --base-url)  BASE_URL="${2%/}"; shift 2 ;;
    --key-id)    KEY_ID="$2"; shift 2 ;;
    --releasever) RELEASEVERS+=("$2"); shift 2 ;;
    --arch)      ARCHES+=("$2"); shift 2 ;;
    --fallback-releasever) FALLBACK_RELEASEVER="$2"; shift 2 ;;
    --no-sign)   SIGN=0; shift ;;
    --repo-username) REPO_USERNAME="$2"; shift 2 ;;
    --repo-password) REPO_PASSWORD="$2"; shift 2 ;;
    --libreac-dir) LIBREAC_DIR="$2"; shift 2 ;;
    --reac-pw-dir) REACPW_DIR="$2"; shift 2 ;;
    -h|--help)   usage ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
done

KEY_FILE_NAME="RPM-GPG-KEY-$REPO_ID"

# ------------------------------------------------------------ preflight ----
command -v createrepo_c >/dev/null || die "createrepo_c not found (dnf install createrepo_c)"
command -v rpm >/dev/null || die "rpm not found"

if [ -z "$FALLBACK_RELEASEVER" ]; then
  FALLBACK_RELEASEVER=$(rpm --eval '%{?fedora}')
fi

if { [ -n "$REPO_USERNAME" ] && [ -z "$REPO_PASSWORD" ]; } || { [ -z "$REPO_USERNAME" ] && [ -n "$REPO_PASSWORD" ]; }; then
  die "--repo-username and --repo-password must both be given or both omitted"
fi

# Signing preflight is deliberately a SEPARATE, up-front gate: everything below
# it is unsigned-tree assembly, so a missing key fails before any work is done
# rather than half way through a publish.
if [ "$SIGN" = 1 ]; then
  command -v rpmsign >/dev/null \
    || die "rpmsign not found -- 'sudo dnf install rpm-sign' (or re-run with --no-sign for an UNSIGNED smoke tree)"
  command -v gpg >/dev/null || die "gpg not found"
  gpg --list-secret-keys "$KEY_ID" >/dev/null 2>&1 \
    || die "no secret key $KEY_ID in this keyring -- import it, or re-run with --no-sign (which produces an UNPUBLISHABLE tree)"
  # gpg-agent holds the passphrase. No passphrase is read, echoed or stored here.
  say "signing key: $KEY_ID (passphrase comes from gpg-agent)"
else
  echo "WARNING: --no-sign -- packages and repomd will NOT be signed and the" >&2
  echo "         emitted .repo will carry gpgcheck=0/repo_gpgcheck=0." >&2
  echo "         This tree is a smoke-test artefact. Do NOT publish it." >&2
fi

# ------------------------------------------------------------ build mode ----
if [ "$BUILD" = 1 ]; then
  [ -d "$LIBREAC_DIR" ] || die "--build: libreac checkout not found at $LIBREAC_DIR (--libreac-dir)"
  [ -d "$REACPW_DIR" ]  || die "--build: reac-pw checkout not found at $REACPW_DIR (--reac-pw-dir)"

  # mod-host first: it depends on nothing else here, and it is the package that makes
  # `dnf install openmixer-full` resolvable at all (openmixer-server requires it and no
  # Fedora repository carries it).
  say "build 1/4: mod-host"
  ( cd "$ROOT" && packaging/build-mod-host-rpm.sh )
  RPM_DIRS+=("$ROOT/build/rpm-mod-host/RPMS" "$ROOT/build/rpm-mod-host/SRPMS")

  say "build 2/4: libreac"
  ( cd "$LIBREAC_DIR" \
    && sh packaging/make-tarball.sh \
    && mkdir -p build/rpm/SOURCES \
    && rpmbuild --define "_topdir $PWD/build/rpm" -ta libreac-*.tar.gz )
  RPM_DIRS+=("$LIBREAC_DIR/build/rpm/RPMS" "$LIBREAC_DIR/build/rpm/SRPMS")

  # reac-pw links the SYSTEM libreac (de-vendored) -- its BuildRequires cannot be
  # satisfied from a tree, only from an installed -devel package.
  if ! pkg-config --exists libreac 2>/dev/null; then
    die "--build: pkgconfig(libreac) is not available, so reac-pw cannot build.
Install the libreac just built, then re-run:
  sudo dnf install $LIBREAC_DIR/build/rpm/RPMS/*/libreac-[0-9]*.rpm $LIBREAC_DIR/build/rpm/RPMS/*/libreac-devel-*.rpm"
  fi

  say "build 3/4: reac-pw"
  ( cd "$REACPW_DIR" \
    && sh packaging/make-tarball.sh \
    && mkdir -p build/rpm/SOURCES \
    && cp reac-pw-*.tar.gz build/rpm/SOURCES/ \
    && rpmbuild --define "_topdir $PWD/build/rpm" -ba packaging/reac-pw.spec )
  RPM_DIRS+=("$REACPW_DIR/build/rpm/RPMS" "$REACPW_DIR/build/rpm/SRPMS")

  say "build 4/4: openmixer"
  ( cd "$ROOT" && scripts/build-rpm.sh )
  RPM_DIRS+=("$ROOT/build/rpm/RPMS" "$ROOT/build/rpm/SRPMS")
fi

[ "${#RPM_DIRS[@]}" -gt 0 ] || die "nothing to publish: pass --rpm-dir DIR (repeatable) or --build"

# ------------------------------------------------------------- collect -----
declare -a FOUND=()
for d in "${RPM_DIRS[@]}"; do
  [ -d "$d" ] || die "--rpm-dir $d is not a directory"
  while IFS= read -r f; do FOUND+=("$f"); done \
    < <(find "$d" -type f -name '*.rpm' | sort)
done
[ "${#FOUND[@]}" -gt 0 ] && say "collected ${#FOUND[@]} rpm(s)" \
  || die "no *.rpm found under: ${RPM_DIRS[*]}"

# rpm 4 records a package signature in SIGPGP/SIGGPG; rpm 6 (Fedora 43+) records
# it in RSAHEADER/DSAHEADER and leaves the old tags at "(none)". Ask for all four
# and treat "anything that is not (none)" as signed, so this works on both.
rpm_is_signed() {
  local sigs
  sigs=$(rpm -qp --qf '%{SIGPGP:pgpsig}|%{SIGGPG:pgpsig}|%{RSAHEADER:pgpsig}|%{DSAHEADER:pgpsig}' \
         "$1" 2>/dev/null) || return 1
  sigs=$(printf '%s' "$sigs" | sed -e 's/(none)//g' -e 's/|//g' -e 's/[[:space:]]//g')
  [ -n "$sigs" ]
}

wanted() { # wanted <needle> <array...>  -- empty filter list means "everything"
  local needle="$1"; shift
  [ $# -eq 0 ] && return 0
  local v; for v in "$@"; do [ "$v" = "$needle" ] && return 0; done
  return 1
}

RPMROOT="$OUT/rpm"
mkdir -p "$RPMROOT"
declare -A TARGET_DIRS=()
PLACED=0
SKIPPED=0

# Read every package's own header once. %{ARCH} on a source rpm reports the BUILD arch
# (x86_64), not "src", so %{SOURCEPACKAGE} is the only reliable discriminator; the
# release is the .fcNN dist tag, never the file name.
declare -a PKG_PATH=() PKG_ARCHDIR=() PKG_RELEASEVER=()
for f in "${FOUND[@]}"; do
  hdr=$(rpm -qp --qf '%{SOURCEPACKAGE}|%{ARCH}|%{RELEASE}' "$f" 2>/dev/null) \
    || { echo "  ! unreadable, skipped: $f" >&2; continue; }
  IFS='|' read -r is_src arch rel <<<"$hdr"
  [ "$is_src" = "1" ] && archdir="SRPMS" || archdir="$arch"
  case "$rel" in
    *.fc[0-9]*) releasever=$(printf '%s\n' "$rel" | sed -n 's/.*\.fc\([0-9]\+\).*/\1/p') ;;
    *)          releasever="$FALLBACK_RELEASEVER" ;;
  esac
  [ -n "$releasever" ] || die "cannot determine a Fedora release for $f -- pass --fallback-releasever N"
  PKG_PATH+=("$f"); PKG_ARCHDIR+=("$archdir"); PKG_RELEASEVER+=("$releasever")
done

# dnf expands $basearch to a concrete arch -- x86_64, aarch64 -- and NEVER falls back to
# a sibling "noarch" directory. A noarch package filed under its own arch name is
# therefore invisible to every client, which is how `dnf install openmixer-full` could
# fail with "no match" while the package sat in the tree. So a noarch package is filed
# into EVERY arch directory of its release, the way Fedora's own repositories carry
# noarch packages inside each arch's Packages/ directory.
#
# The arch set of a release is what this publish produces plus what the tree already
# holds, so re-publishing only the noarch half still reaches the arches already there.
declare -A ARCHES_FOR=()
add_arch() { # add_arch <releasever> <arch>
  case " ${ARCHES_FOR[$1]:-} " in *" $2 "*) ;; *) ARCHES_FOR[$1]="${ARCHES_FOR[$1]:-} $2" ;; esac
}
for i in "${!PKG_PATH[@]}"; do
  case "${PKG_ARCHDIR[$i]}" in
    noarch|SRPMS) ;;
    *) add_arch "${PKG_RELEASEVER[$i]}" "${PKG_ARCHDIR[$i]}" ;;
  esac
done
for d in "$RPMROOT"/fedora/*/*/; do
  [ -d "$d" ] || continue
  a=$(basename "$d"); r=$(basename "$(dirname "$d")")
  case "$a" in noarch|SRPMS) continue ;; esac
  add_arch "$r" "$a"
done

place() { # place <file> <releasever> <archdir>
  local f=$1 releasever=$2 archdir=$3 dest src_id dst_id
  wanted "$releasever" "${RELEASEVERS[@]+"${RELEASEVERS[@]}"}" || return 1
  wanted "$archdir" "${ARCHES[@]+"${ARCHES[@]}"}" || return 1
  dest="$RPMROOT/fedora/$releasever/$archdir"
  mkdir -p "$dest"
  TARGET_DIRS["$dest"]=1
  # Idempotent: the same package already there -> leave it alone, signature and all.
  # The comparison is on SHA256HEADER (the immutable header digest), NOT on file
  # bytes: signing rewrites the signature header, so a byte compare against the
  # still-unsigned build output would re-copy and re-sign on every single run.
  if [ -f "$dest/$(basename "$f")" ]; then
    src_id=$(rpm -qp --qf '%{SHA256HEADER}' "$f" 2>/dev/null || true)
    dst_id=$(rpm -qp --qf '%{SHA256HEADER}' "$dest/$(basename "$f")" 2>/dev/null || true)
    if [ -n "$src_id" ] && [ "$src_id" = "$dst_id" ]; then return 0; fi
    if [ -z "$src_id" ] && cmp -s "$f" "$dest/$(basename "$f")"; then return 0; fi
  fi
  install -m 0644 "$f" "$dest/"
}

for i in "${!PKG_PATH[@]}"; do
  f="${PKG_PATH[$i]}"; archdir="${PKG_ARCHDIR[$i]}"; releasever="${PKG_RELEASEVER[$i]}"
  if [ "$archdir" = "noarch" ]; then
    read -r -a arches <<<"${ARCHES_FOR[$releasever]:-}"
    [ "${#arches[@]}" -gt 0 ] || die "noarch package $f has no arch directory to go in for Fedora $releasever.
A noarch package is filed inside each arch's directory, because dnf's \$basearch never
resolves to 'noarch'. Publish an arch-specific package for this release in the same run,
or publish into a tree that already has one."
    placed_any=0
    for a in "${arches[@]}"; do
      place "$f" "$releasever" "$a" && placed_any=1
    done
    [ "$placed_any" = 1 ] && PLACED=$((PLACED + 1)) || SKIPPED=$((SKIPPED + 1))
  else
    place "$f" "$releasever" "$archdir" && PLACED=$((PLACED + 1)) || SKIPPED=$((SKIPPED + 1))
  fi
done

say "placed $PLACED rpm(s) into $RPMROOT/fedora (filtered out: $SKIPPED)"
[ "$PLACED" -gt 0 ] || die "every collected package was filtered out by --releasever/--arch"

# --------------------------------------------------------------- signing ---
# Everything below the SIGN guard is the separable signing path: with --no-sign
# the tree is byte-identical minus signatures and the .asc/.repo flags.
if [ "$SIGN" = 1 ]; then
  say "signing packages with $KEY_ID"
  for dest in "${!TARGET_DIRS[@]}"; do
    for f in "$dest"/*.rpm; do
      [ -f "$f" ] || continue
      # already signed -- re-signing would only churn bytes on every re-run
      rpm_is_signed "$f" && continue
      rpmsign --define "_gpg_name $KEY_ID" --addsign "$f" >/dev/null \
        || die "rpmsign failed on $f (is gpg-agent running and the key unlocked?)"
    done
  done

  # Prove it, rather than assume it: every package must now carry a signature.
  for dest in "${!TARGET_DIRS[@]}"; do
    for f in "$dest"/*.rpm; do
      [ -f "$f" ] || continue
      rpm_is_signed "$f" || die "unsigned after rpmsign: $f"
    done
  done
  say "all packages carry a signature"
fi

# ------------------------------------------------------------- repodata ----
for dest in "${!TARGET_DIRS[@]}"; do
  if [ -d "$dest/repodata" ]; then
    createrepo_c --update --quiet "$dest"
  else
    createrepo_c --quiet "$dest"
  fi
  say "repodata: ${dest#$OUT/}"
  if [ "$SIGN" = 1 ]; then
    rm -f "$dest/repodata/repomd.xml.asc"
    gpg --batch --yes --armor --detach-sign --local-user "$KEY_ID" \
        --output "$dest/repodata/repomd.xml.asc" "$dest/repodata/repomd.xml" \
      || die "detach-signing repomd.xml failed in $dest"
  else
    rm -f "$dest/repodata/repomd.xml.asc"
  fi
done

# ------------------------------------------------------- key + .repo file --
if [ "$SIGN" = 1 ]; then
  gpg --armor --export "$KEY_ID" > "$RPMROOT/$KEY_FILE_NAME"
  [ -s "$RPMROOT/$KEY_FILE_NAME" ] || die "exporting the public key $KEY_ID produced an empty file"
elif [ -f "$TEMPLATE_DIR/$KEY_FILE_NAME" ]; then
  install -m 0644 "$TEMPLATE_DIR/$KEY_FILE_NAME" "$RPMROOT/$KEY_FILE_NAME"
fi

gpgcheck=$([ "$SIGN" = 1 ] && echo 1 || echo 0)
# Basic Auth lines, only when both were given -- a private target (an R2 bucket behind a Worker
# demanding Basic Auth) needs them; a public tree (GitHub Pages) has none and these are empty.
auth_lines=""
if [ -n "$REPO_USERNAME" ]; then
  auth_lines=$'username='"$REPO_USERNAME"$'\npassword='"$REPO_PASSWORD"
fi
cat > "$RPMROOT/$REPO_ID.repo" <<EOF
# $REPO_NAME -- Fedora package repository
# Generated by packaging/publish-repo.sh (task #320). Edit the template at
# packaging/repo/$REPO_ID.repo, not this copy.
[$REPO_ID]
name=$REPO_NAME
baseurl=$BASE_URL/fedora/\$releasever/\$basearch/
enabled=1
gpgcheck=$gpgcheck
repo_gpgcheck=$gpgcheck
gpgkey=$BASE_URL/$KEY_FILE_NAME
metadata_expire=6h
skip_if_unavailable=False
${auth_lines:+$auth_lines}

[$REPO_ID-source]
name=$REPO_NAME -- Sources
baseurl=$BASE_URL/fedora/\$releasever/SRPMS/
enabled=0
gpgcheck=$gpgcheck
repo_gpgcheck=$gpgcheck
gpgkey=$BASE_URL/$KEY_FILE_NAME
${auth_lines:+$auth_lines}
EOF

# GitHub Pages must not run Jekyll over the tree: it would drop repodata/ dirs
# and anything else it considers a "special" path.
touch "$OUT/.nojekyll"

say "tree ready: $OUT"
find "$OUT" -maxdepth 4 -mindepth 1 \( -name repodata -prune -o -print \) | sed "s|^$OUT|  .|" | sort
if [ "$SIGN" = 1 ]; then
  echo
  echo "Consumers install with:"
  echo "  sudo dnf config-manager addrepo --from-repofile=$BASE_URL/$REPO_ID.repo"
  echo "  sudo rpm --import $BASE_URL/$KEY_FILE_NAME"
  # openmixer-full hard-requires reac-pw. Naming it unconditionally printed an install
  # line that fails whenever the transport is not in the tree, so the line names the
  # metapackage this tree can actually resolve.
  if find "$RPMROOT" -name 'reac-pw-*.rpm' -print -quit | grep -q .; then
    echo "  sudo dnf install openmixer-full"
  else
    echo "  sudo dnf install openmixer"
    echo
    echo "(openmixer-full needs reac-pw, which this tree does not carry -- publish the"
    echo " REAC transport into it before pointing anyone at openmixer-full.)"
  fi
else
  echo
  echo "UNSIGNED tree -- not publishable. Re-run without --no-sign."
fi
