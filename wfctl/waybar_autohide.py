#!/usr/bin/env python3
"""
waybar_autohide - inteligentne skryvanie Waybaru vo Wayfire cez IPC.

Pravidla:
  * Ak nejake okno zasahuje do oblasti baru (--bar-height px od okraja
    fokusovaneho vystupu; hore alebo dole podla --position), bar sa skryje.
  * Skryty bar sa zobrazi, ked kurzor dojde na okraj obrazovky, na ktorom je
    bar (--edge px), a to len v strednej casti okraja o sirke --edge-width %
    (predvolene 80 %; rohy ostavaju volne pre hotcorners).
  * Bar ostane zobrazeny, kym je kurzor v jeho oblasti (--bar-height px), a hned
    ako kurzor oblast opusti, sa znova skryje (ak okno stale zasahuje do baru).
  * Ak ziadne okno do oblasti baru nezasahuje, bar je stale zobrazeny.
  * Skript posiela Waybaru signal len pri zmene stavu.
  * Skript sa ukonci spolu s rodicom (Waybar), ak je spusteny ako jeho modul.
  
Parametre:
    --bar-height	40	    Výška Waybaru v px. Určuje oblasť, v ktorej bar ostane zobrazený, aj pásmo, do ktorého musí okno zasahovať, aby sa bar skryl.
    --position	    top	    Na ktorom okraji je bar: top alebo bottom.
    --edge	        2	    Výška spúšťacej zóny na okraji v px. Skrytý bar sa zobrazí, keď je kurzor v poslednom N px okraja. Ak dáš rovnakú hodnotu ako --bar-height, zobrazí sa už pri vstupe do celej oblasti baru.
    --edge-width	80	    Nový parameter. Šírka spúšťacej zóny v % šírky obrazovky, vycentrovaná.
    --margin	    0	    Okno sa počíta ako zasahujúce aj vtedy, ak je do N px pod pásmom baru. Hodnota 1 pomáha, keď je "exclusive": true a maximalizované okná sa dotýkajú baru, ale doň nezasahujú.
    --poll	        0.03	Ako často (v sekundách) sa číta poloha kurzora, kým nejaké okno zasahuje do baru.
    --ignore	    prázdne	app-id okna, ktoré sa ignoruje. Dá sa opakovať, napr. --ignore conky --ignore wf-background.
    --process	    waybar	Názov procesu baru, ktorému sa posielajú signály.
    --hide-signal	SIGUSR1	Signál na skrytie.
    --show-signal	SIGUSR2	Signál na zobrazenie.
    --startup-delay	1	    Pauza na začiatku v sekundách, kým si Waybar nainštaluje handlery signálov. Pri pomalom štarte ju zvýš.

Potrebuje:
  * plugins `ipc` a `ipc-rules` vo wayfire.ini; ipc-rules musi mat metodu
    window-rules/get_cursor_position (patchnuty ipc-rules / Wayfire >= 0.10),
  * Waybar s    "on-sigusr1": "hide",  "on-sigusr2": "show",
  * odporucane v konfigu Waybaru  "exclusive": false  - inak Wayfire pri
    zobrazenom bare zmensi maximalizovane okna pod barom a tie do jeho oblasti
    nezasahuju (bar by sa potom nikdy neskryl). Alternativa: --margin 1.
Ziadne externe python zavislosti.
"""
import argparse
import ctypes
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

# udalosti, po ktorych treba prepocitat, ci okno zasahuje do baru
RELEVANT = {
    "view-mapped", "view-unmapped", "view-tiled", "view-fullscreened",
    "view-minimized", "view-focused", "view-set-output",
    "view-geometry-changed", "view-wset-changed", "view-workspace-changed",
    "wset-workspace-changed", "output-gain-focus", "output-wset-changed",
    "view-sticky",
}


class Ipc:
    def __init__(self):
        self.path = os.environ.get("WAYFIRE_SOCKET")
        if not self.path:
            sys.exit("WAYFIRE_SOCKET nie je nastaveny (bezi Wayfire s pluginmi ipc + ipc-rules?)")
        self.sock = None
        self.connect()

    def connect(self):
        if self.sock:
            self.sock.close()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.path)

    def send(self, method, data=None):
        raw = json.dumps({"method": method, "data": data or {}}).encode()
        self.sock.sendall(struct.pack("=I", len(raw)) + raw)

    def _exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("Wayfire zatvoril IPC socket")
            buf += chunk
        return buf

    def recv(self):
        (n,) = struct.unpack("=I", self._exact(4))
        return json.loads(self._exact(n))

    def call(self, method, data=None):
        """Request/response na trvalom spojeni; pri chybe jeden pokus o znovupripojenie."""
        for attempt in (0, 1):
            try:
                self.send(method, data)
                return self.recv()
            except (OSError, EOFError):
                if attempt:
                    raise
                self.connect()


def cursor_pos(req):
    pos = req.call("window-rules/get_cursor_position").get("pos")
    return (pos["x"], pos["y"]) if pos else None


def obstructed(req, out, args):
    """True, ak niektore okno na aktivnom workspace zasahuje do pasma baru."""
    w, h = out["geometry"]["width"], out["geometry"]["height"]
    band = args.bar_height + args.margin
    for v in req.call("window-rules/list-views"):
        if v.get("type") != "toplevel" or not v.get("mapped") or v.get("minimized"):
            continue
        if v.get("output-id") != out["id"]:
            continue
        if v.get("app-id") in args.ignore:
            continue
        g = v["geometry"]
        # geometria je lokalna pre vystup; aktualny workspace je [0,w) x [0,h)
        cx, cy = g["x"] + g["width"] / 2, g["y"] + g["height"] / 2
        if not (v.get("sticky") or (0 <= cx < w and 0 <= cy < h)):
            continue
        if args.position == "top":
            in_band = g["y"] < band and g["y"] + g["height"] > 0
        else:
            in_band = g["y"] + g["height"] > h - band and g["y"] < h
        if in_band and g["x"] < w and g["x"] + g["width"] > 0:
            return True
    return False


def cursor_zones(out, pos, args):
    """(v oblasti baru, na okraji baru) pre kurzor v globalnych suradniciach."""
    g = out["geometry"]
    cx, cy = pos[0] - g["x"], pos[1] - g["y"]
    inside_x = 0 <= cx < g["width"]
    # spustacia zona na okraji je vycentrovana a siaha len cez --edge-width % sirky
    # (rohy zostavaju volne pre hotcorners); oblast baru je vzdy cela sirka
    margin_x = g["width"] * (1 - args.edge_width / 100) / 2
    inside_edge_x = margin_x <= cx < g["width"] - margin_x
    if args.position == "top":
        d = cy  # kurzor tlaceny k hornemu okraju: cy = 0
        def in_zone(n): return 0 <= d < n
    else:
        # kurzor tlaceny k spodku: cy je tesne pod height (nie height-1), preto d tesne nad 0
        d = g["height"] - cy
        def in_zone(n): return 0 < d <= n
    return (inside_x and in_zone(args.bar_height),
            inside_edge_x and in_zone(args.edge))


def bar(sig, name):
    subprocess.run(["pkill", f"-{sig}", "-x", name], check=False)


def die_with_parent():
    """Jadro posle SIGTERM tomuto procesu, ked zanikne rodic (Waybar)."""
    try:
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except OSError:
        pass


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bar-height", type=float, default=40, help="vyska baru v px (predvolene 40)")
    p.add_argument("--position", choices=("top", "bottom"), default="top",
                   help="na ktorom okraji obrazovky je bar (predvolene top)")
    p.add_argument("--edge", type=float, default=2,
                   help="skryty bar sa zobrazi, ked je kurzor v poslednych N px okraja obrazovky (predvolene 2; "
                        "nastav rovnake ako --bar-height, aby sa zobrazil uz pri vstupe do celej oblasti)")
    p.add_argument("--edge-width", type=float, default=30,
                   help="sirka spustacej zony na okraji v %% sirky obrazovky, vycentrovana (predvolene 80; "
                        "rohy zostanu volne pre hotcorners). Oblast, v ktorej bar ostane zobrazeny, "
                        "je vzdy cela sirka")
    p.add_argument("--margin", type=float, default=0,
                   help="okno sa pocita ako zasahujuce, ak zasahuje do pasma bar-height+margin px od okraja "
                        "(1 = aj okno dotykajuce sa baru)")
    p.add_argument("--poll", type=float, default=0.03, help="interval citania pozicie kurzora v sekundach")
    p.add_argument("--ignore", action="append", default=[], help="app-id okna, ktore sa ignoruje (opakovatelne)")
    p.add_argument("--process", default="waybar", help="nazov procesu baru")
    p.add_argument("--hide-signal", default="SIGUSR1")
    p.add_argument("--show-signal", default="SIGUSR2")
    p.add_argument("--startup-delay", type=float, default=1,
                   help="pauza na zaciatku, kym si Waybar nainstaluje handlery signalov (predvolene 1 s)")
    args = p.parse_args()
    args.edge_width = max(0.0, min(100.0, args.edge_width))

    die_with_parent()
    parent = os.getppid()
    time.sleep(args.startup_delay)

    req = Ipc()
    probe = req.call("window-rules/get_cursor_position")
    if "pos" not in probe:
        sys.exit("ipc-rules nepodporuje window-rules/get_cursor_position "
                 f"({probe.get('error', 'neznama odpoved')}) - nainstaluj patchnuty plugin ipc-rules.")

    watch = Ipc()
    watch.send("window-rules/events/watch")  # bez zoznamu = vsetky udalosti
    watch.recv()

    state = {"hidden": None, "obstructed": False, "out": None}

    def recompute():
        out = req.call("window-rules/get-focused-output").get("info")
        state["out"] = out
        state["obstructed"] = bool(out) and obstructed(req, out, args)

    def want_hidden():
        if not state["obstructed"]:
            return False
        pos = cursor_pos(req)
        if pos is None:
            return bool(state["hidden"])
        in_area, in_edge = cursor_zones(state["out"], pos, args)
        # skryty bar sa ukaze az na okraji obrazovky; zobrazeny ostane, kym je kurzor v jeho oblasti
        keep_visible = in_edge if state["hidden"] else in_area
        return not keep_visible

    def apply(hidden):
        if hidden == state["hidden"]:
            return
        state["hidden"] = hidden
        bar(args.hide_signal if hidden else args.show_signal, args.process)

    def cleanup(*_):
        # show posli len ked Waybar stale zije; inak by SIGUSR2 mohol ukoncit
        # novu instanciu Waybaru, ktora este nema nainstalovane handlery
        if os.getppid() == parent:
            bar(args.show_signal, args.process)
        sys.exit(0)

    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)

    recompute()
    apply(want_hidden())
    while True:
        if os.getppid() != parent:
            sys.exit(0)
        # kurzor sa cita len ked nejake okno zasahuje do baru; inak cakaj na udalosti
        # (najviac 5 s, aby sa dala skontrolovat existencia rodica)
        timeout = args.poll if state["obstructed"] else 5
        ready, _, _ = select.select([watch.sock], [], [], timeout)
        dirty = False
        budget = 50  # nech povodna zaplava udalosti nezablokuje citanie kurzora
        while ready and budget:
            budget -= 1
            if watch.recv().get("event") in RELEVANT:
                dirty = True
            # zluc rychlu sekvenciu udalosti do jedneho prepoctu
            ready, _, _ = select.select([watch.sock], [], [], 0.03 if dirty else 0)
        if dirty:
            recompute()
        apply(want_hidden())


if __name__ == "__main__":
    try:
        main()
    except (EOFError, OSError) as e:
        sys.exit(f"IPC spojenie s Wayfire zlyhalo: {e}")
