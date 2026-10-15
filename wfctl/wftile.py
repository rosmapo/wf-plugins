#!/usr/bin/env python3
"""
wftile.py - jednoduchý tiling cez wfctl pre aktívny workspace.

2 okná: fokusované vľavo, druhé vpravo
        cyklus pri opakovaní: vľavo/vpravo -> hore/dole -> restore -> znova vľavo/vpravo
        (vľavo -> hore, vpravo -> dole)
3 okná: fokusované vľavo, ostatné vpravo hore a vpravo dole
4 okná: fokusované vľavo hore, ostatné vľavo dole, vpravo hore, vpravo dole
3 a 4 okná: opakované spustenie vráti okná do pôvodných pozícií (grid/restore)

Použitie:  wftile.py            (cesta k wfctl sa dá zmeniť cez env WFCTL)
           wftile.py -n         (iba vypíše, čo by urobil)
"""

import json
import os
import subprocess
import sys

WFCTL = os.environ.get("WFCTL", "wfctl")
DRY_RUN = "-n" in sys.argv[1:] or "--dry-run" in sys.argv[1:]


def wfctl(*args, check=True):
    """Zavolá wfctl a vráti naparsovaný JSON (alebo None pri prázdnom výstupe)."""
    try:
        proc = subprocess.run(
            [WFCTL, *args], capture_output=True, text=True
        )
    except FileNotFoundError:
        print(f"Chyba: príkaz '{WFCTL}' sa nenašiel.", file=sys.stderr)
        sys.exit(1)

    if proc.returncode != 0:
        if check:
            print(proc.stderr.strip() or "wfctl zlyhal", file=sys.stderr)
            sys.exit(1)
        return None

    out = proc.stdout.strip()
    if not out:
        return None
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def unwrap(obj, key):
    """Niektoré odpovede sú zabalené v {'info': {...}}, iné nie."""
    if isinstance(obj, dict) and key in obj and isinstance(obj[key], dict):
        return obj[key]
    return obj


def center_in_rect(geo, rect):
    cx = geo["x"] + geo["width"] / 2
    cy = geo["y"] + geo["height"] / 2
    return (
        rect["x"] <= cx < rect["x"] + rect["width"]
        and rect["y"] <= cy < rect["y"] + rect["height"]
    )


# Pozícia slotu ako (x, y, šírka, výška) v zlomkoch použiteľnej plochy
SLOT_FRACTIONS = {
    "slot_l": (0, 0, 0.5, 1),
    "slot_r": (0.5, 0, 0.5, 1),
    "slot_t": (0, 0, 1, 0.5),
    "slot_b": (0, 0.5, 1, 0.5),
    "slot_tl": (0, 0, 0.5, 0.5),
    "slot_tr": (0.5, 0, 0.5, 0.5),
    "slot_bl": (0, 0.5, 0.5, 0.5),
    "slot_br": (0.5, 0.5, 0.5, 0.5),
}


def window_in_slot(view, slot, area, tol=0.1):
    """Zhoduje sa geometria okna so slotom (s toleranciou na medzery a dekorácie)?"""
    fx, fy, fw, fh = SLOT_FRACTIONS[slot]
    g = view["geometry"]
    aw, ah = area["width"], area["height"]
    return (
        abs(g["x"] - (area["x"] + fx * aw)) <= tol * aw
        and abs(g["y"] - (area["y"] + fy * ah)) <= tol * ah
        and abs(g["width"] - fw * aw) <= tol * aw
        and abs(g["height"] - fh * ah) <= tol * ah
    )


def matches_layout(windows, slots, area):
    """Je každý zo slotov obsadený práve jedným oknom (v ľubovoľnom poradí)?"""
    free = list(windows)
    for slot in slots:
        hit = next((w for w in free if window_in_slot(w, slot, area)), None)
        if hit is None:
            return False
        free.remove(hit)
    return True


def apply_slots(plan, focused_id):
    """plan = [(view, slot), ...]; po rozmiestnení vráti fokus pôvodnému oknu."""
    for view, slot in plan:
        vid = view["id"]
        print(f"{vid:>5}  {view.get('app-id', '?'):<20} -> {slot}")
        if DRY_RUN:
            continue
        # grid plugin berie id okna; posielame obe bežné názvy kľúča
        wfctl(f"grid/{slot}", f"id={vid}", f"view_id={vid}")
    if not DRY_RUN:
        wfctl("window-rules/focus-view", f"id={focused_id}", check=False)


def main():
    # Fokusovaný výstup a jeho geometria
    out_info = wfctl("window-rules/get-focused-output")
    out_info = unwrap(out_info, "info")
    if not out_info or "id" not in out_info:
        print("Nepodarilo sa zistiť fokusovaný výstup.", file=sys.stderr)
        sys.exit(1)
    output_id = out_info["id"]

    # Ak chýba plná info o výstupe, dotiahneme ju
    if "geometry" not in out_info:
        out_info = unwrap(wfctl("window-rules/output-info", f"id={output_id}"), "info")
    out_geo = out_info["geometry"]
    # Pohľad na aktívny workspace = obdĺžnik (0, 0, w, h) vo view súradniciach
    ws_rect = {"x": 0, "y": 0, "width": out_geo["width"], "height": out_geo["height"]}
    # Použiteľná plocha (bez Waybaru), ak ju Wayfire vráti
    area = out_info.get("workarea") or {
        "x": 0, "y": 0, "width": out_geo["width"], "height": out_geo["height"]
    }

    # Fokusované okno
    focused = unwrap(wfctl("window-rules/get-focused-view", check=False), "info")
    focused_id = focused.get("id") if isinstance(focused, dict) else None

    # Zoznam okien na aktívnom workspace
    views = wfctl("window-rules/list-views") or []
    windows = []
    for v in views:
        if v.get("type") != "toplevel" or not v.get("mapped", True):
            continue
        if v.get("role", "toplevel") != "toplevel":
            continue
        if v.get("output-id", output_id) != output_id:
            continue
        if v.get("minimized") or v.get("state", {}).get("minimized"):
            continue
        geo = v.get("geometry")
        if not geo or not center_in_rect(geo, ws_rect):
            continue
        windows.append(v)

    count = len(windows)
    if count < 2 or count > 4:
        print(f"Na aktívnom workspace je {count} okien - tiling sa robí pre 2 až 4.")
        return

    # Fokusované okno; ak nie je medzi nimi, vezmeme prvé
    ids = [w["id"] for w in windows]
    if focused_id not in ids:
        focused_id = ids[0]

    layouts = {
        2: ["slot_l", "slot_r"],
        3: ["slot_l", "slot_tr", "slot_br"],
        4: ["slot_tl", "slot_bl", "slot_tr", "slot_br"],
    }

    # Opakované spustenie: okná už sú v dlaždiciach -> ďalší krok cyklu
    if count == 2:
        if matches_layout(windows, ["slot_l", "slot_r"], area):
            # 2. spustenie: vľavo -> hore, vpravo -> dole
            first, second = sorted(windows, key=lambda w: w["geometry"]["x"])
            apply_slots([(first, "slot_t"), (second, "slot_b")], focused_id)
            return
        if matches_layout(windows, ["slot_t", "slot_b"], area):
            # 3. spustenie: návrat do pôvodných (stacking) pozícií
            apply_slots([(w, "restore") for w in windows], focused_id)
            return
    elif matches_layout(windows, layouts[count], area):
        # 2. spustenie pri 3 a 4 oknách: návrat do pôvodných pozícií
        apply_slots([(w, "restore") for w in windows], focused_id)
        return

    # Fokusované okno ide na prvé miesto
    main = next(w for w in windows if w["id"] == focused_id)

    # Ostatné zoradíme podľa aktuálnej polohy (zhora, zľava) kvôli stabilite
    others = sorted(
        (w for w in windows if w["id"] != focused_id),
        key=lambda w: (w["geometry"]["y"], w["geometry"]["x"]),
    )

    slots = layouts[count]

    apply_slots(list(zip([main, *others], slots)), focused_id)


if __name__ == "__main__":
    main()
