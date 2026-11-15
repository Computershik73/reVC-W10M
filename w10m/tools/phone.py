"""Talk to the phone's Windows Device Portal (USB: http://127.0.0.1:10080) about the reVC port.

    python w10m/tools/phone.py install [--appx FILE]   install or update (default: the newest in w10m/builds)
    python w10m/tools/phone.py version                 the installed package
    python w10m/tools/phone.py launch | stop           start / terminate the app
    python w10m/tools/phone.py log                     LocalState/revc.log (and the previous run's) -> w10m/work
    python w10m/tools/phone.py file NAME               any file from LocalState -> w10m/work/phone_NAME
                                                       (frame.bmp also as a PNG)
    python w10m/tools/phone.py rm NAME                 delete a file in LocalState
    python w10m/tools/phone.py run [--seconds N]       a run nobody holds the phone for: stop, start the game
                                                       at once, wait, fetch the log (and frame.bmp if it is there)
    python w10m/tools/phone.py ls [PATH]               what is in LocalState (PATH: \\LocalState\\game ...)
    python w10m/tools/phone.py mem [--watch SEC]       memory of the running app
    python w10m/tools/phone.py env NAME=value          a setting for the next start (see below)
    python w10m/tools/phone.py unenv NAME=value        take it back
    python w10m/tools/phone.py phones                  the phones on USB, and which one the USB portal is

The phone needs Developer mode with the Device Portal on. Adapted from butterscotch-w10m's
phone.py and deploy.py, which have the experience behind the comments.

Which phone: with several on USB the PC forwards the portal's ports to one of them only, and
not always the same one. Name the other by its Wi-Fi portal, before the command
(Settings > Update & security > For developers shows the address), or in REVC_PORTAL:

    python w10m/tools/phone.py --portal http://192.168.31.137 install

What is fetched from a phone named this way goes to w10m/work/phone-<address>, not to w10m/work.
install, run, launch and stop say which phone they are talking to.

env: the app cannot open a file the portal uploads into its LocalState, it only sees its name,
so a setting is an empty file called "env.NAME=value"; the app puts those into its environment
when it starts. REVC_ANGLE_FL=9_3 caps Direct3D (OpenGL ES 2.0, what most phones have) on a
phone that has more; ALSOFT_LOGLEVEL=3 makes OpenAL Soft talk. REVC_FRAMESHOT=5 saves the game's
picture as frame.bmp every five seconds and REVC_KEYS=13@20,40@24 presses keys by the clock
(w10m/README.md has the list).
"""
import argparse
import base64
import glob
import http.cookiejar
import json
import os
import re
import ssl
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

# A phone is on USB or on the local network, never behind a proxy: a VPN client's HTTP(S)_PROXY
# on the PC turns every request to the portal into a 503 or into nothing (husk-w10m)
for _var in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
    os.environ.pop(_var, None)
os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"

# Over USB the portal answers on http :10080 and/or https :10443 (self-signed); whichever is up wins
USB_PORTALS = ["http://127.0.0.1:10080", "https://127.0.0.1:10443"]
PORTALS = list(USB_PORTALS)
PORTAL = PORTALS[0]
_INSECURE = ssl._create_unverified_context()
PACKAGE_PREFIX = env.IDENTITY + "_"
CURL = os.environ.get("CURL", r"C:\Windows\System32\curl.exe")
OUT = env.WORK


def use_portal(url):
    """Another phone than the one on the USB ports: its own portal, and its own folder for what is
    fetched from it and for what is remembered about it (the installed package)."""
    global PORTALS, PORTAL, OUT
    url = url.rstrip("/")
    if "://" not in url:
        url = "http://" + url
    PORTALS = [url]
    PORTAL = url
    OUT = os.path.join(env.WORK, "phone-" + re.sub(r"[^A-Za-z0-9.-]", "_", urllib.parse.urlsplit(url).netloc))


if os.environ.get("REVC_PORTAL"):
    use_portal(os.environ["REVC_PORTAL"])


def who():
    """Says which phone this is: with two on USB, the ports may be the other one's."""
    info = json.loads(get("/api/os/info"))
    print(f"phone: {info.get('ComputerName')} (Windows 10 Mobile {info.get('OsVersion', '?').split('.arm')[0]}) at {PORTAL}")


def get(endpoint, timeout=30, patience=45, **params):
    """GET from whichever port is up. The portal (or the USB tunnel to it) drops connections for
    half a minute at a time, at once and without a word: asked again for `patience` seconds."""
    global PORTAL
    query = ("?" + urllib.parse.urlencode(params) if params else "")
    error = None
    deadline = time.time() + patience
    while True:
        for base in [PORTAL] + [p for p in PORTALS if p != PORTAL]:
            try:
                with urllib.request.urlopen(base + endpoint + query, timeout=timeout, context=_INSECURE) as r:
                    PORTAL = base
                    return r.read()
            except urllib.error.HTTPError:
                raise
            except OSError as e:  # connection refused: try the other port
                error = e
        if time.time() > deadline:
            raise error
        time.sleep(3)


def send(method, endpoint, data=b"", headers=None, **params):
    """POST/DELETE: the portal wants the CSRF-Token cookie echoed back as X-CSRF-Token."""
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar),
                                         urllib.request.HTTPSHandler(context=_INSECURE))
    get("/api/os/info")  # find a live port
    opener.open(PORTAL + "/api/os/info", timeout=30).read()
    token = next((c.value for c in jar if c.name == "CSRF-Token"), "")
    req = urllib.request.Request(PORTAL + endpoint + "?" + urllib.parse.urlencode(params), data=data,
                                 method=method, headers=dict(headers or {}, **{"X-CSRF-Token": token}))
    with opener.open(req, timeout=60) as r:
        return r.status


def upload(endpoint, name, path):
    """A multipart POST with curl (the portal's uploads are large and its errors readable).
    Returns curl's output with the HTTP status on its last line."""
    jar = os.path.join(OUT, "portal_cookies.txt")
    os.makedirs(OUT, exist_ok=True)
    get("/api/os/info")
    subprocess.run([CURL, "-s", "-k", "--noproxy", "*", "-c", jar, "-o", os.devnull, f"{PORTAL}/api/os/info"])
    token = ""
    with open(jar, encoding="utf-8", errors="replace") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) >= 7 and fields[5] == "CSRF-Token":
                token = fields[6]
    r = subprocess.run([CURL, "-s", "-k", "--noproxy", "*", "-m", "1800", "-b", jar, "-H", f"X-CSRF-Token: {token}",
                        "-w", "\nHTTP %{http_code}", "-X", "POST",
                        "-F", f"{name}=@{path};type=application/octet-stream", f"{PORTAL}{endpoint}"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout.strip() or r.stderr.strip()


def packages():
    # enumerating packages can take a minute when the phone is busy
    pkgs = json.loads(get("/api/app/packagemanager/packages", timeout=120))["InstalledPackages"]
    return [p for p in pkgs if p["PackageFullName"].startswith(PACKAGE_PREFIX)]


def package(refresh=False):
    """The installed port. Remembered between calls (w10m/work/phone_package.json, written by
    install and version): listing the phone's packages takes a minute or more when it is busy."""
    cache = os.path.join(OUT, "phone_package.json")
    if not refresh and os.path.isfile(cache):
        with open(cache, encoding="utf-8") as f:
            return json.load(f)
    found = packages()
    if not found:
        if os.path.isfile(cache):
            os.remove(cache)
        sys.exit("the port is not installed on the phone")
    os.makedirs(OUT, exist_ok=True)
    with open(cache, "w", encoding="utf-8") as f:
        json.dump({k: found[0].get(k) for k in ("PackageFullName", "PackageRelativeId")}, f)
    return found[0]


def installed(tries=1):
    """Installed package name, None if absent, "?" if the portal is unreachable right now."""
    for attempt in range(tries):
        try:
            found = packages()
            return found[0]["PackageFullName"] if found else None
        except OSError:  # phone asleep, rebooting or unplugged; or the portal dropped this one request, as it does
            if attempt + 1 < tries:
                time.sleep(5)
    return "?"


def newest_appx():
    found = sorted(glob.glob(os.path.join(env.OUT, "reVC_*_ARM.appx")), key=os.path.getmtime)
    if not found:
        sys.exit(f"no package in {env.OUT}: python w10m/tools/build.py")
    return found[-1]


def cmd_install(a):
    appx = a.appx or newest_appx()
    with zipfile.ZipFile(appx) as z:
        identity = re.search(r"<Identity\b[^>]*>", z.read("AppxManifest.xml").decode("utf-8")).group(0)
    version = re.search(r'Version="([^"]+)"', identity).group(1)
    who()
    if installed(tries=30) == "?":
        sys.exit("the Device Portal does not answer (is the phone on USB, unlocked, with the portal on?)")
    name = os.path.basename(appx)
    print(f"installing {name}")
    out = upload("/api/app/packagemanager/package?package=" + urllib.parse.quote(name), name, appx)
    print("upload:", out.replace("\n", " "))
    if "HTTP 202" not in out:
        sys.exit("the phone did not accept the package")
    deadline = time.time() + a.wait
    while time.time() < deadline:
        time.sleep(5)
        try:
            body = get("/api/app/packagemanager/state")
        except OSError as e:
            # 204 while still installing comes back as an empty body; 404 means no install is running
            print("state:", e)
            continue
        if not body:
            continue
        state = json.loads(body)
        print("state:", state.get("Reason"), "-", state.get("CodeText", "").strip())
        if not state.get("Success"):
            sys.exit("the phone did not install the package (is Computershik.cer trusted on it?)")
        break
    while time.time() < deadline:
        current = installed()
        if current and current != "?" and f"_{version}_" in current:
            package(refresh=True)
            print("installed:", current)
            return
        time.sleep(5)
    sys.exit(f"the phone did not report version {version} within {a.wait} s (last: {installed()})")


def cmd_version(_):
    print(package(refresh=True)["PackageFullName"])


def cmd_launch(_):
    who()
    p = package()
    if not p.get("PackageRelativeId"):
        p = package(refresh=True)
    b64 = lambda t: base64.b64encode(t.encode()).decode()  # noqa: E731
    status = send("POST", "/api/taskmanager/app", appid=b64(p["PackageRelativeId"]), package=b64(p["PackageFullName"]))
    print("launched" if status == 200 else f"launch: HTTP {status}", p["PackageFullName"])


def cmd_stop(_):
    who()
    p = package()
    status = send("DELETE", "/api/taskmanager/app", package=base64.b64encode(p["PackageFullName"].encode()).decode())
    print("stopped" if status == 200 else f"stop: HTTP {status}")


def fetch(pfn, name, path="\\LocalState"):
    return get("/api/filesystem/apps/file", timeout=120, knownfolderid="LocalAppData", packagefullname=pfn,
               filename=name, path=path)


def cmd_log(_):
    pfn = package()["PackageFullName"]
    os.makedirs(OUT, exist_ok=True)
    # revc.prev.log holds the previous run (rotated when the app starts)
    for name in ("revc.prev.log", "revc.log"):
        try:
            data = fetch(pfn, name)
        except OSError:
            continue
        out = os.path.join(OUT, "phone_" + name)
        with open(out, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes -> {out}")


def bmp_to_png(data):
    """The 24-bit bottom-up BMP the app's REVC_FRAMESHOT writes, as a PNG (None if it is not one)."""
    if data[:2] != b"BM" or len(data) < 54 or struct.unpack_from("<H", data, 28)[0] != 24:
        return None
    start, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    stride = (width * 3 + 3) & ~3
    if width <= 0 or height <= 0 or len(data) < start + stride * height:
        return None
    rows = bytearray()
    for y in range(height - 1, -1, -1):
        row = data[start + y * stride:start + y * stride + width * 3]
        rgb = bytearray(row)
        rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
        rows += b"\0" + rgb

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def save(name, data):
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, "phone_" + name)
    with open(out, "wb") as f:
        f.write(data)
    print(f"{name}: {len(data)} bytes -> {out}")
    png = bmp_to_png(data) if name.lower().endswith(".bmp") else None
    if png:
        with open(out[:-4] + ".png", "wb") as f:
            f.write(png)
        print(f"{name}: as a PNG -> {out[:-4]}.png")


def cmd_file(a):
    save(a.name, fetch(package()["PackageFullName"], a.name, a.path))


def cmd_frames(a):
    """The series REVC_FRAMESHOT=2k left (frame_001.bmp...): fetched, taken off the phone, and put on
    one sheet, frames.png, each with its number (needs PIL; without it the PNGs alone)."""
    pfn = package()["PackageFullName"]
    listing = json.loads(get("/api/filesystem/apps/files", knownfolderid="LocalAppData", packagefullname=pfn,
                             path="\\LocalState"))
    names = sorted(i["Name"] for i in listing.get("Items", []) if re.fullmatch(r"frame_\d+\.bmp", i["Name"]))
    folder = os.path.join(OUT, "frames")
    if os.path.isdir(folder):
        for old in os.listdir(folder):
            os.remove(os.path.join(folder, old))
    os.makedirs(folder, exist_ok=True)
    pictures = []
    for name in names[::a.every]:
        png = bmp_to_png(fetch(pfn, name))
        if png:
            path = os.path.join(folder, name[:-4] + ".png")
            with open(path, "wb") as f:
                f.write(png)
            pictures.append((int(name[6:-4]), path))
    for name in names:
        remove(pfn, name)
    print(f"{len(pictures)} of {len(names)} frames -> {folder}")
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    if not pictures:
        return
    first = Image.open(pictures[0][1])
    w, h = first.size
    columns = a.columns
    rows = (len(pictures) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * w, rows * h))
    draw = ImageDraw.Draw(sheet)
    for i, (number, path) in enumerate(pictures):
        x, y = (i % columns) * w, (i // columns) * h
        sheet.paste(Image.open(path), (x, y))
        draw.rectangle([x, y, x + 30, y + 12], fill=(0, 0, 0))
        draw.text((x + 2, y), str(number), fill=(255, 255, 0))
    out = os.path.join(OUT, "frames.png")
    sheet.save(out)
    print(f"sheet: {out} ({columns} across, {w}x{h} each)")


def remove(pfn, name, path="\\LocalState"):
    try:
        return send("DELETE", "/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=pfn,
                    filename=name, path=path)
    except urllib.error.HTTPError as e:
        return e.code


def cmd_rm(a):
    status = remove(package()["PackageFullName"], a.name, a.path)
    print(f"{a.name}: " + ("removed" if status == 200 else f"HTTP {status}"))


def cmd_run(a):
    """Stop the app, take away what makes it ask before it starts the game (the marker of a start
    that did not reach the menu) and the last run's picture, start it, wait, fetch what it left."""
    p = package()
    if not p.get("PackageRelativeId"):
        p = package(refresh=True)
    pfn = p["PackageFullName"]
    try:
        cmd_stop(a)
    except urllib.error.HTTPError as e:
        print(f"stop: HTTP {e.code}")
    time.sleep(2)
    for name in ("starting", "frame.bmp"):
        remove(pfn, name)
    cmd_launch(a)
    time.sleep(a.seconds)
    cmd_log(a)
    try:
        save("frame.bmp", fetch(pfn, "frame.bmp"))
    except OSError:
        pass  # no REVC_FRAMESHOT, or no frame yet


def cmd_ls(a):
    pfn = package()["PackageFullName"]
    listing = json.loads(get("/api/filesystem/apps/files", knownfolderid="LocalAppData", packagefullname=pfn, path=a.path))
    for item in listing.get("Items", []):
        folder = item.get("Type", 0) & 16
        print(f"{'<dir>' if folder else item.get('FileSize', 0):>12}  {item['Name']}")


def cmd_mem(a):
    while True:
        procs = json.loads(get("/api/resourcemanager/processes"))["Processes"]
        game = [p for p in procs if p.get("PackageFullName", "").startswith(PACKAGE_PREFIX)]
        if not game:
            print("the app is not running")
        for p in game:
            mb = lambda k: p.get(k, 0) / 2**20  # noqa: E731
            print(f"{time.strftime('%H:%M:%S')} private={mb('PrivateWorkingSet'):.0f} MB "
                  f"working={mb('WorkingSetSize'):.0f} MB virtual={mb('VirtualSize'):.0f} MB "
                  f"cpu={p.get('CPUUsage', 0):.0f}%")
        if not a.watch:
            break
        time.sleep(a.watch)


def flag_name(setting):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=[^\\/:*?\"<>|]*", setting):
        sys.exit("NAME=value, with a value that can be part of a file name")
    return "env." + setting


def listed(pfn, name):
    listing = json.loads(get("/api/filesystem/apps/files", knownfolderid="LocalAppData", packagefullname=pfn,
                             path="\\LocalState"))
    return any(item["Name"] == name for item in listing.get("Items", []))


def cmd_env(a):
    pfn = package()["PackageFullName"]
    name = flag_name(a.setting)
    # An empty file, as a multipart body made by hand: curl's -F cuts the field's name at its "="
    boundary = uuid.uuid4().hex
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n\r\n--{boundary}--\r\n").encode()
    try:
        send("POST", "/api/filesystem/apps/file", data=body,
             headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
             knownfolderid="LocalAppData", packagefullname=pfn, path="\\LocalState")
    except urllib.error.HTTPError as e:
        print(f"the portal answered HTTP {e.code}")  # some phones write the file and answer 500 anyway
    if not listed(pfn, name):
        sys.exit(f"the phone did not take {name}")
    print(f"{name}: set (read when the app next starts)")


def cmd_phones(_):
    """The phones on USB as WinAppDeployCmd sees them, and the one the USB portal belongs to."""
    tools = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x86\WinAppDeployCmd.exe"))
    # 10.0.16299's is the one known to talk to Windows 10 Mobile (husk-w10m)
    tool = next((t for t in tools if "10.0.16299.0" in t), tools[-1] if tools else None)
    if tool:
        out = subprocess.run([tool, "devices"], capture_output=True, text=True, timeout=180).stdout
        for line in out.splitlines():
            fields = line.split("\t")
            if len(fields) >= 3 and re.fullmatch(r"[0-9a-f-]{36}", fields[1].strip()):
                print(f"on USB: {fields[-1].strip()}  ({fields[1].strip()})")
    try:
        who()
    except OSError as e:
        print(f"no portal at {PORTAL}: {e}")


def cmd_unenv(a):
    pfn = package()["PackageFullName"]
    name = flag_name(a.setting)
    status = send("DELETE", "/api/filesystem/apps/file", knownfolderid="LocalAppData", packagefullname=pfn,
                  filename=name, path="\\LocalState")
    print(f"{name}: " + ("removed" if status == 200 else f"HTTP {status}"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--portal", help="another phone's Device Portal, e.g. http://192.168.31.137 (default: the one on USB)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ip = sub.add_parser("install")
    ip.add_argument("--appx")
    ip.add_argument("--wait", type=int, default=600, help="seconds to wait for the phone to finish")
    for name in ("version", "launch", "stop", "log", "phones"):
        sub.add_parser(name)
    sp = sub.add_parser("frames")
    sp.add_argument("--every", type=int, default=1, help="every so many of the frames")
    sp.add_argument("--columns", type=int, default=5)
    fp = sub.add_parser("file")
    fp.add_argument("name")
    fp.add_argument("--path", default="\\LocalState")
    lp = sub.add_parser("ls")
    lp.add_argument("path", nargs="?", default="\\LocalState")
    mp = sub.add_parser("mem")
    mp.add_argument("--watch", type=float, default=0)
    rp = sub.add_parser("rm")
    rp.add_argument("name")
    rp.add_argument("--path", default="\\LocalState")
    np = sub.add_parser("run")
    np.add_argument("--seconds", type=float, default=45, help="how long the app is left to run before its log is fetched")
    for name in ("env", "unenv"):
        ep = sub.add_parser(name)
        ep.add_argument("setting", metavar="NAME=value")
    a = ap.parse_args()
    if a.portal:
        use_portal(a.portal)
    try:
        {"install": cmd_install, "version": cmd_version, "launch": cmd_launch, "stop": cmd_stop, "log": cmd_log,
         "file": cmd_file, "ls": cmd_ls, "mem": cmd_mem, "env": cmd_env, "unenv": cmd_unenv, "rm": cmd_rm,
         "run": cmd_run, "phones": cmd_phones, "frames": cmd_frames}[a.cmd](a)
    except urllib.error.HTTPError as e:
        sys.exit(f"the Device Portal answered HTTP {e.code} ({e.reason})")
    except OSError as e:
        sys.exit(f"the Device Portal does not answer: {e}")


if __name__ == "__main__":
    main()
