"""Builds dist/<App>-pacote.zip in the 3DShub layout:
<App>/<App>.3dsx, <App>/<App>.cia, <App>/LEIA-ME.txt, <App>/pc/...
Usage: python package.py <project dir> <App name>
"""
import os
import sys
import zipfile

root, app = sys.argv[1], sys.argv[2]
dist = os.path.join(root, "dist")
os.makedirs(dist, exist_ok=True)
out = os.path.join(dist, f"{app}-pacote.zip")

files = [(f"{app}.3dsx", f"{app}.3dsx"), (f"{app}.cia", f"{app}.cia"), ("LEIA-ME.txt", "LEIA-ME.txt")]
pc = os.path.join(root, "pc")
if os.path.isdir(pc):
    for dirpath, dirnames, names in os.walk(pc):
        dirnames[:] = [d for d in dirnames if d not in ("radios", "__pycache__")]
        for n in names:
            full = os.path.join(dirpath, n)
            files.append((os.path.relpath(full, root), os.path.relpath(full, root)))

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for src, arc in files:
        z.write(os.path.join(root, src), f"{app}/{arc}".replace("\\", "/"))
print(out)
