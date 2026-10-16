#!/usr/bin/env python3
"""
wftile.py - jednoduchý tiling cez wfctl pre aktívny workspace.

wftile.py           dlaždice: fokusované okno je hlavné
    2 okná: fokusované vľavo, druhé vpravo
    3 okná: fokusované vľavo, ostatné vpravo hore a vpravo dole
    4 okná: fokusované vľavo hore, ostatné vľavo dole, vpravo hore, vpravo dole

    Ak už sú okná v dlaždiciach (aj po stack alebo rotate), vráti ich do
    pôvodných pozícií (grid/restore).

wftile.py stack     prepína medzi horizontálnym a vertikálnym rozložením (bez restore)
    2 okná: vľavo/vpravo <-> hore/dole
    3 okná: veľké vľavo + dve vpravo <-> veľké hore + dve pod ním
    4 okná: mriežka 2x2 v oboch prípadoch (nič sa nemení)
    Okná mimo dlaždíc sa rozložia vertikálne (fokusované hore).

wftile.py rotate    posunie okná o jedno miesto v aktuálnom rozložení
                    (2 okná = výmena, 3 a 4 okná = cyklus po smere hodín)

wftile.py status    JSON pre Waybar: ikona podľa stavového súboru
                    $XDG_RUNTIME_DIR/wftile.state, prázdny text, ak súbor neexistuje
    Každá zmena rozloženia pošle Waybaru signál SIGRTMIN+8 (env WFTILE_SIGNAL).

wftile.py watch     strážca (spúšťa sa sám pri dlaždicovaní): počúva udalosti Wayfiru
                    a keď sa zatvorí niektoré z dlaždicových okien, ostatné okamžite
                    vráti do pôvodných pozícií (restore), zmaže stav a pošle signál.
                    Ladenie udalostí: WFTILE_DEBUG=1 (výpis do $XDG_RUNTIME_DIR/wftile.log)

Voľby:  -n, --dry-run   iba vypíše, čo by urobil
Cestu k wfctl možno zmeniť cez env WFCTL.
"""

import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys

WFCTL = os.environ.get("WFCTL", "wfctl")
DRY_RUN = False

# Stavový súbor (existuje, kým sú okná v dlaždiciach) a signál pre Waybar
RUNTIME_DIR = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
STATE_FILE = os.path.join(RUNTIME_DIR, "wftile.state")
PID_FILE = os.path.join(RUNTIME_DIR, "wftile.pid")
LOG_FILE = os.path.join(RUNTIME_DIR, "wftile.log")
WAYBAR_SIGNAL = os.environ.get("WFTILE_SIGNAL", "8")  # musí sedieť s "signal" v Waybare

ICONS = {"tile": "", "stack": ""}
TOOLTIPS = {
    "tile": "Okná v dlaždiciach (horizontálne)",
    "stack": "Okná v dlaždiciach (vertikálne)",
}

# Rozloženia podľa režimu a počtu okien; prvý slot patrí hlavnému (fokusovanému) oknu
LAYOUTS = {
    "tile": {
        2: ["slot_l", "slot_r"],
        3: ["slot_l", "slot_tr", "slot_br"],
        4: ["slot_tl", "slot_bl", "slot_tr", "slot_br"],
    },
    "stack": {
        2: ["slot_t", "slot_b"],
        3: ["slot_t", "slot_bl", "slot_br"],
        4: ["slot_tl", "slot_bl", "slot_tr", "slot_br"],
    },
}

# Poradie slotov po smere hodinových ručičiek pre rotate
CLOCKWISE = {
    ("tile", 2): ["slot_l", "slot_r"],
    ("tile", 3): ["slot_l", "slot_tr", "slot_br"],
    ("tile", 4): ["slot_tl", "slot_tr", "slot_br", "slot_bl"],
    ("stack", 2): ["slot_t", "slot_b"],
    ("stack", 3): ["slot_t", "slot_br", "slot_bl"],
    ("stack", 4): ["slot_tl", "slot_tr", "slot_br", "slot_bl"],
}

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


def assign_to_slots(windows, slots, area):
    """Vráti okná zoradené podľa slotov, alebo None, ak sloty nie sú obsadené 1:1."""
    free = list(windows)
    ordered = []
    for slot in slots:
        hit = next((w for w in free if window_in_slot(w, slot, area)), None)
        if hit is None:
            return None
        free.remove(hit)
        ordered.append(hit)
    return ordered


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


def collect_workspace():
    """Zistí použiteľnú plochu, okná na aktívnom workspace a fokusované okno."""
    out_info = unwrap(wfctl("window-rules/get-focused-output"), "info")
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
    area = out_info.get("workarea") or ws_rect

    focused = unwrap(wfctl("window-rules/get-focused-view", check=False), "info")
    focused_id = focused.get("id") if isinstance(focused, dict) else None

    windows = []
    for v in wfctl("window-rules/list-views") or []:
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

    ids = [w["id"] for w in windows]
    if windows and focused_id not in ids:
        focused_id = ids[0]
    return area, windows, focused_id


def read_state():
    """Vráti {'mode': 'tile'|'stack', 'ids': [id okien]} alebo None."""
    try:
        with open(STATE_FILE) as f:
            st = json.load(f)
    except (OSError, ValueError):
        return None
    if isinstance(st, dict) and st.get("mode") in ICONS:
        st.setdefault("ids", [])
        return st
    return None


def remove_state_file():
    try:
        os.remove(STATE_FILE)
    except OSError:
        pass


def notify_waybar():
    try:
        subprocess.run(
            ["pkill", f"-RTMIN+{WAYBAR_SIGNAL}", "-x", "waybar"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        pass


def watcher_pid():
    """PID bežiaceho strážcu (príkaz 'watch') alebo None."""
    try:
        with open(PID_FILE) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            cmdline = f.read().decode(errors="replace")
    except (OSError, ValueError):
        return None
    return pid if "wftile" in cmdline and "watch" in cmdline else None


def start_watcher():
    """Spustí strážcu na pozadí (ak ešte nebeží)."""
    if watcher_pid():
        return
    subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "watch"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def stop_watcher():
    pid = watcher_pid()
    if pid and pid != os.getpid():
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def set_state(mode, windows=None):
    """Zapíše stav (mode + id okien) alebo ho zmaže (None); pošle Waybaru signál.

    Pri zápise sa spustí strážca, pri zmazaní sa zastaví."""
    if DRY_RUN:
        return
    if mode:
        try:
            with open(STATE_FILE, "w") as f:
                json.dump({"mode": mode, "ids": [w["id"] for w in windows or []]}, f)
        except OSError:
            pass
        start_watcher()
    else:
        remove_state_file()
        stop_watcher()
    notify_waybar()


def do_status():
    """Výstup pre Waybar (JSON): ikona, ak existuje stavový súbor, inak prázdny text."""
    st = read_state()
    if st:
        print(json.dumps({
            "text": ICONS[st["mode"]],
            "class": st["mode"],
            "tooltip": TOOLTIPS[st["mode"]] + "\nĽavý klik: rotate, pravý klik: stack",
        }, ensure_ascii=False))
    else:
        print(json.dumps({"text": "", "class": "none"}))


def log(obj):
    """Ladiaci výpis do $XDG_RUNTIME_DIR/wftile.log, ak je nastavené WFTILE_DEBUG."""
    if os.environ.get("WFTILE_DEBUG"):
        with open(LOG_FILE, "a") as f:
            f.write(json.dumps(obj, ensure_ascii=False) + "\n")


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Wayfire prerušil spojenie.")
        buf += chunk
    return buf


def read_msg(sock):
    (length,) = struct.unpack("<I", recv_exact(sock, 4))
    return json.loads(recv_exact(sock, length))


def close_tiled(st, closed_id):
    """Zatvorilo sa dlaždicové okno: ostatné vráti do pôvodných pozícií."""
    for vid in st["ids"]:
        if vid != closed_id:
            wfctl("grid/restore", f"id={vid}", f"view_id={vid}", check=False)
    set_state(None)


def do_watch():
    """Strážca: počúva udalosti Wayfiru; pri zatvorení dlaždicového okna urobí restore."""
    existing = watcher_pid()
    if existing and existing != os.getpid():
        return  # už jeden beží
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))

    def on_term(*_):
        sys.exit(0)

    signal.signal(signal.SIGTERM, on_term)
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(os.environ["WAYFIRE_SOCKET"])
            req = json.dumps({"method": "window-rules/events/watch", "data": {}}).encode()
            sock.sendall(struct.pack("<I", len(req)) + req)
            while True:
                ev = read_msg(sock)
                log(ev)
                if ev.get("event") != "view-unmapped":
                    continue
                vid = (ev.get("view") or {}).get("id", ev.get("id"))
                st = read_state()
                if not st:
                    break  # stav medzitým zrušený (restore)
                if vid in st["ids"]:
                    close_tiled(st, vid)
                    break
    except (KeyError, OSError, ValueError) as e:
        log({"watch-error": str(e)})
        remove_state_file()
        notify_waybar()
    finally:
        if watcher_pid() == os.getpid():
            try:
                os.remove(PID_FILE)
            except OSError:
                pass


def do_tile(windows, area, focused_id):
    """Dlaždice; ak už sú okná v niektorom rozložení (tile aj stack), urobí restore."""
    count = len(windows)
    for mode in ("tile", "stack"):
        if assign_to_slots(windows, LAYOUTS[mode][count], area):
            apply_slots([(w, "restore") for w in windows], focused_id)
            set_state(None)
            return
    place("tile", windows, focused_id)


def do_stack(windows, area, focused_id):
    """Prepína medzi horizontálnym (tile) a vertikálnym (stack) rozložením."""
    count = len(windows)
    for cur, other in (("tile", "stack"), ("stack", "tile")):
        ordered = assign_to_slots(windows, LAYOUTS[cur][count], area)
        if ordered:
            # Zachová roly okien (hlavné okno ostane hlavné aj po rotate)
            apply_slots(list(zip(ordered, LAYOUTS[other][count])), focused_id)
            set_state(other, windows)
            return
    place("stack", windows, focused_id)


def place(mode, windows, focused_id):
    """Rozloží okná z neusporiadaného stavu; fokusované je hlavné."""
    slots = LAYOUTS[mode][len(windows)]
    main = next(w for w in windows if w["id"] == focused_id)
    # Ostatné zoradíme podľa aktuálnej polohy kvôli stabilite poradia
    if mode == "stack":
        key = lambda w: (w["geometry"]["x"], w["geometry"]["y"])
    else:
        key = lambda w: (w["geometry"]["y"], w["geometry"]["x"])
    others = sorted((w for w in windows if w["id"] != focused_id), key=key)

    apply_slots(list(zip([main, *others], slots)), focused_id)
    set_state(mode, windows)


def do_rotate(windows, area, focused_id):
    """Posunie okná o jedno miesto v rozložení, v ktorom práve sú."""
    count = len(windows)
    for mode in ("tile", "stack"):
        slots = LAYOUTS[mode][count]
        ordered = assign_to_slots(windows, slots, area)
        if ordered:
            # Posun po smere hodinových ručičiek (nie v poradí LAYOUTS)
            cycle = CLOCKWISE[(mode, count)]
            target = {s: cycle[(cycle.index(s) + 1) % count] for s in cycle}
            plan = [(w, target[s]) for w, s in zip(ordered, slots)]
            apply_slots(plan, focused_id)
            st = read_state()
            set_state(st["mode"] if st else mode, windows)
            return
    print("Okná nie sú v dlaždiciach - najprv spusti wftile.py alebo wftile.py stack.")


def main():
    global DRY_RUN
    parser = argparse.ArgumentParser(
        description="Tiling 2 až 4 okien na aktívnom workspace cez wfctl.",
        epilog="Opakované spustenie bez argumentu vráti okná do pôvodných pozícií.",
    )
    parser.add_argument(
        "command", nargs="?", default="tile", choices=["tile", "stack", "rotate", "status", "watch"],
        help="tile (predvolené): dlaždice, opakovane = restore | "
             "stack: prepína horizontálne/vertikálne | rotate: posun okien",
    )
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="iba vypíše, čo by urobil")
    args = parser.parse_args()
    DRY_RUN = args.dry_run

    # status a watch nepotrebujú zoznam okien
    if args.command == "status":
        do_status()
        return
    if args.command == "watch":
        do_watch()
        return

    area, windows, focused_id = collect_workspace()

    count = len(windows)
    if count < 2 or count > 4:
        print(f"Na aktívnom workspace je {count} okien - tiling sa robí pre 2 až 4.")
        return

    if args.command == "rotate":
        do_rotate(windows, area, focused_id)
    elif args.command == "stack":
        do_stack(windows, area, focused_id)
    else:
        do_tile(windows, area, focused_id)


if __name__ == "__main__":
    main()
