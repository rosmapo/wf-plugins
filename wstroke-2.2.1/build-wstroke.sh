#!/usr/bin/env bash
# build-wstroke.sh -- zostaví wstroke (release, stripped) alebo vytvorí .deb balík.
#
# Spúšťaj z koreňového adresára zdrojov (tam, kde je meson.build a debian/):
#   ./build-wstroke.sh          # skompiluje do ./build, stripped plugin skopíruje do ./out/
#   ./build-wstroke.sh deb      # vytvorí .deb balíky (výsledok v rodičovskom adresári)
set -euo pipefail

mode="${1:-build}"

die()  { echo "CHYBA: $*" >&2; exit 1; }
warn() { echo "VAROVANIE: $*" >&2; }
need() { command -v "$1" >/dev/null 2>&1 || die "chýba príkaz '$1' (skús: $2)"; }

[[ -f meson.build && -d src ]] \
    || die "spusti skript v koreňovom adresári zdrojov wstroke (tam, kde je meson.build)"

# Kontrola, že sú aplikované úpravy (len upozornenie, nič nezastaví)
grep -q "key_timer"    src/easystroke_gestures.cpp 2>/dev/null || warn "chýba patch sendkey-delay (key_timer)"
grep -q "click_timer"  src/easystroke_gestures.cpp 2>/dev/null || warn "chýba patch click-hold (click_timer)"
grep -q "events.frame" src/input_events.cpp        2>/dev/null || warn "chýba patch pointer-frame (events.frame)"

case "$mode" in
    build)
        need meson  "sudo apt install meson"
        need ninja  "sudo apt install ninja-build"
        need valac  "sudo apt install valac"

        rm -rf build
        meson setup build --prefix=/usr --buildtype=release
        meson compile -C build

        lib="$(find build -name 'libwstroke.so' -print -quit)"
        [[ -n "$lib" ]] || die "libwstroke.so sa nenašiel v ./build"

        mkdir -p out
        cp "$lib" out/libwstroke.so
        strip --strip-unneeded out/libwstroke.so

        echo
        echo "Hotovo. Stripped plugin: $(pwd)/out/libwstroke.so"
        ls -lh out/libwstroke.so
        echo
        plugindir="$(pkg-config --variable=plugindir wayfire 2>/dev/null || true)"
        echo "Nahradenie systémového pluginu (najprv urob zálohu pôvodného):"
        echo "  sudo cp ${plugindir:-<plugindir>}/libwstroke.so ~/libwstroke.so.bak"
        echo "  sudo cp out/libwstroke.so ${plugindir:-<plugindir>}/libwstroke.so"
        echo "Potom sa odhlás a prihlás (reštart Wayfire)."
        ;;

    deb)
        need dpkg-buildpackage "sudo apt install dpkg-dev"
        need fakeroot          "sudo apt install fakeroot"
        need dpkg-checkbuilddeps "sudo apt install dpkg-dev"
        [[ -d debian ]] || die "chýba adresár debian/ (zdroje z 'apt source wstroke')"

        if ! missing="$(dpkg-checkbuilddeps 2>&1)"; then
            echo "$missing" >&2
            die "chýbajú build závislosti. Doinštaluj ich: sudo apt build-dep ./"
        fi

        # Knižnice mimo balíkového systému (napr. wlroots zostavená do /usr/local)
        # nemajú pre dpkg-shlibdeps informácie o balíku a build by zlyhal. Preto sa
        # do debian/rules pridá override, ktorý takéto knižnice len preskočí; knižnice
        # z balíkov dostanú závislosti ako zvyčajne.
        if ! grep -q 'override_dh_shlibdeps' debian/rules; then
            warn "pridávam do debian/rules override_dh_shlibdeps (--ignore-missing-info); záloha: ../wstroke-debian-rules.orig"
            cp -n debian/rules ../wstroke-debian-rules.orig
            printf '\noverride_dh_shlibdeps:\n\tdh_shlibdeps -- --ignore-missing-info\n' >> debian/rules
        fi

        if [[ -n "${DEB_BUILD_OPTIONS:-}" ]]; then
            export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS} parallel=$(nproc)"
        else
            export DEB_BUILD_OPTIONS="parallel=$(nproc)"
        fi
        dpkg-buildpackage -us -uc -b

        echo
        echo "Hotovo. Vytvorené balíky:"
        ls -lh ../wstroke*.deb
        echo
        echo "Inštalácia:   sudo apt install ../wstroke_*.deb"
        echo "Aby apt balík neprepísal verziou z repozitára:   sudo apt-mark hold wstroke"
        ;;

    *)
        die "neznámy režim '$mode' (použi: build alebo deb)"
        ;;
esac
