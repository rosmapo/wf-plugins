#!/usr/bin/env python3

import os
import sys
import socket
import struct
import json
import subprocess

SOCKET_PATH = os.environ.get("WAYFIRE_SOCKET")

def wayfire_request(method, data=None):
    if data is None:
        data = {}
    message = json.dumps({"method": method, "data": data}).encode()
    packet = struct.pack("<I", len(message)) + message

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(SOCKET_PATH)
        sock.sendall(packet)
        header = sock.recv(4)
        if len(header) != 4:
            raise RuntimeError("Wayfire nevrátil platnú odpoveď.")
        length = struct.unpack("<I", header)[0]
        response = b""
        while len(response) < length:
            chunk = sock.recv(length - len(response))
            if not chunk:
                raise RuntimeError("Wayfire prerušil spojenie.")
            response += chunk
    
    result = json.loads(response)
    if "error" in result:
        raise RuntimeError(result["error"])
    return result

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Použitie: skript.py <app-id> [príkaz na spustenie]")
        sys.exit(1)

    app_target = sys.argv[1].lower()
    launch_command = sys.argv[2] if len(sys.argv) > 2 else app_target

    focused = False

    if SOCKET_PATH:
        try:
            # 1. Získame zoznam okien cez IPC
            result = wayfire_request("window-rules/list-views")
            # Wayfire môže vracať zoznam priamo alebo pod kľúčom, závisí od verzie, 
            # ale z vášho predchádzajúceho skriptu vieme, že to vracalo zoznam (views) priamo:
            views = result if isinstance(result, list) else result.get("views", [])

            for v in views:
                app_id = v.get("app-id", "").lower()
                # Porovnáme, či app-id okna zodpovedá hľadanej aplikácii
                if app_target in app_id:
                    view_id = v["id"]
                    # 2. Ak okno existuje, prepneme naň fokus
                    wayfire_request("window-rules/focus-view", {"id": view_id})
                    focused = True
                    break
        except Exception as e:
            # Ak IPC zlyhá, ignorujeme a skúsim aplikáciu normálne spustiť
            pass

    # 3. Ak okno nebolo nájdené, spustíme aplikáciu štandardne
    if not focused:
        subprocess.Popen(launch_command, shell=True)