#!/usr/bin/env bash
# build-plugin-deb.sh - zostavi jeden Wayfire plugin (napr. rounded-corners) ako samostatny .deb
#
# Pouzitie (v chroote alebo systeme, kde je nainstalovany novy wayfire + wayfire-dev
# + libwf-config-dev z baliku z build-wayfire-debs.sh):
#   ./build-plugin-deb.sh NAZOV ZDROJAKY_DIR [--out DIR]
#
#   NAZOV        napr. rounded-corners  -> balik wayfire-plugin-rounded-corners
#   ZDROJAKY_DIR adresar s meson.build, ALEBO adresar s jednym .cpp suborom
#                (+ volitelne NAZOV.xml s metadatami pluginu)
#
# Env: PLUGIN_VER (default 1.0), CXXSTD (default c++20), DEB_REV (default 0local1)

set -Eeuo pipefail
die() { echo "CHYBA: $*" >&2; exit 1; }

[[ $# -ge 2 ]] || die "pouzitie: $0 NAZOV ZDROJAKY_DIR [--out DIR]"
NAME=$1; SRC=$(readlink -f "$2"); shift 2
OUT=$PWD/debs
while [[ $# -gt 0 ]]; do
  case $1 in
    --out) OUT=$2; shift 2 ;;
    *) die "neznamy argument $1" ;;
  esac
done
[[ -d $SRC ]] || die "$SRC nie je adresar"

PLUGIN_VER=${PLUGIN_VER:-1.0}
DEB_REV=${DEB_REV:-0local1}
CXXSTD=${CXXSTD:-c++20}
PKG=${PKGNAME:-wayfire-plugin-$NAME}
VER="${PLUGIN_VER}-${DEB_REV}"
ARCH=$(dpkg --print-architecture)
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

pkg-config --exists wayfire || die "pkg-config nevidi wayfire.pc (nainstaluj wayfire-dev a libwf-config-dev)"
WFVER=$(pkg-config --modversion wayfire)
WFMAJMIN=${WFVER%.*}
WFNEXT=$(awk -F. '{print $1"."$2+1}' <<<"$WFVER")

PLUGINDIR=$(pkg-config --variable=plugindir wayfire 2>/dev/null || true)
METADIR=$(pkg-config --variable=metadatadir wayfire 2>/dev/null || true)
PLUGINDIR=${PLUGINDIR:-/usr/lib/$MULTIARCH/wayfire}
METADIR=${METADIR:-/usr/share/wayfire/metadata}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STAGE=$WORK/stage
mkdir -p "$STAGE$PLUGINDIR" "$STAGE$METADIR"

if [[ -f $SRC/meson.build ]]; then
  echo "==> meson build"
  meson setup "$WORK/build" "$SRC" --prefix=/usr --buildtype=release
  ninja -C "$WORK/build"
  meson install -C "$WORK/build" --destdir "$STAGE"
else
  mapfile -t cpps < <(find "$SRC" -maxdepth 1 -name '*.cpp')
  [[ ${#cpps[@]} -ge 1 ]] || die "v $SRC nie je meson.build ani .cpp"
  echo "==> g++ (${cpps[*]##*/})"
  # shellcheck disable=SC2046
  g++ -shared -fPIC -O2 -std="$CXXSTD" -DWLR_USE_UNSTABLE -DWAYFIRE_PLUGIN \
      -o "$STAGE$PLUGINDIR/lib${NAME}.so" "${cpps[@]}" \
      $(pkg-config --cflags --libs wayfire wf-config)
  if [[ -f $SRC/$NAME.xml ]]; then
    install -Dm644 "$SRC/$NAME.xml" "$STAGE$METADIR/$NAME.xml"
  elif [[ -d $SRC/metadata ]]; then
    cp "$SRC"/metadata/*.xml "$STAGE$METADIR/"
  fi
fi

elfs=()
while IFS= read -r -d '' f; do
  if file -b "$f" | grep -q '^ELF'; then
    elfs+=("$f")
    strip --strip-unneeded "$f" 2>/dev/null || true
  fi
done < <(find "$STAGE" -type f -print0)
rmdir --ignore-fail-on-non-empty "$STAGE$METADIR" 2>/dev/null || true

# systemove zavislosti z ELF suborov
mkdir -p "$WORK/shl/debian"
printf 'Source: p\nMaintainer: x <x@localhost>\n\nPackage: p\nArchitecture: any\n' > "$WORK/shl/debian/control"
SHDEPS=""
if [[ ${#elfs[@]} -gt 0 ]]; then
  # libwf-utils si nesie wayfire balik; zavislost na Debian libwf-utils0t64 vyradime
  SHDEPS=$( ( cd "$WORK/shl" && dpkg-shlibdeps -O --ignore-missing-info "${elfs[@]}" 2>/dev/null \
            | sed -n 's/^shlibs:Depends=//p' \
            | tr ',' '\n' | sed 's/^ *//; s/ *$//' | grep -v '^libwf-utils' \
            | paste -sd, - | sed 's/,/, /g' ) || true )
fi
DEPS="wayfire (>= ${WFMAJMIN}), wayfire (<< ${WFNEXT})"
[[ -z $SHDEPS ]] || DEPS="$DEPS, $SHDEPS"

mkdir -p "$STAGE/DEBIAN" "$OUT"
{
  echo "Package: $PKG"
  echo "Version: $VER"
  echo "Architecture: $ARCH"
  echo "Maintainer: Local build <root@localhost>"
  echo "Section: x11"
  echo "Priority: optional"
  echo "Depends: $DEPS"
  # REPLACES="wstroke" -> balik nahradi povodny Debian balik toho isteho pluginu
  if [[ -n ${REPLACES:-} ]]; then echo "Conflicts: $REPLACES"; echo "Replaces: $REPLACES"; fi
  echo "Description: Wayfire plugin $NAME (local build against wayfire $WFVER)"
} > "$STAGE/DEBIAN/control"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUT/${PKG}_${VER}_${ARCH}.deb" >/dev/null
echo "Hotovo: $OUT/${PKG}_${VER}_${ARCH}.deb  (zavisi od wayfire >= ${WFMAJMIN}, << ${WFNEXT})"
