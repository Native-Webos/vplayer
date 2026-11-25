# SPDX-License-Identifier: GPL-3.0-or-later
# tools/make-manifest.py IPK TAG OUT.json : the Homebrew Channel release manifest for a release's .ipk.
#
#   python3 tools/make-manifest.py Pylon-0.1.0-webos-arm.ipk v0.1.0 com.sostk.vplayer.manifest.json
#
# The manifest goes into the GitHub release beside the .ipk (release asset
# com.sostk.vplayer.manifest.json): the apps-repo registration (packaging/com.sostk.vplayer.yml) points
# at releases/latest/download/com.sostk.vplayer.manifest.json, so a new full (not pre-) release is
# picked up without changing the registration. The id and version are read from the package itself
# (its control file), so the manifest, control and appinfo always agree; the hash is of that exact file.
import hashlib
import io
import json
import os
import sys
import tarfile

REPO = "https://github.com/Native-Webos/vplayer"


def ar_members(data):
    """The members of an ar archive (an .ipk), name -> bytes."""
    assert data[:8] == b"!<arch>\n", "not an ar archive"
    out, pos = {}, 8
    while pos + 60 <= len(data):
        head = data[pos:pos + 60]
        name = head[:16].decode().strip().rstrip("/")
        size = int(head[48:58].decode().strip())
        out[name] = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    return out


def main():
    ipk, tag, out = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(ipk, "rb").read()
    control = tarfile.open(fileobj=io.BytesIO(ar_members(data)["control.tar.gz"]))
    fields = {}
    for member in control.getmembers():
        if os.path.basename(member.name) == "control":
            for line in control.extractfile(member).read().decode().splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    fields[k.strip()] = v.strip()
    version = fields["Version"]
    assert tag == "v" + version, f"tag {tag} does not match the package version {version}"
    manifest = {
        "id": fields["Package"],
        "version": version,
        "type": "native",
        "title": "Pylon",
        "appDescription": "VLC's playback engine as a native webOS app",
        "iconUri": "https://raw.githubusercontent.com/Native-Webos/vplayer/main/app/packaging/icon160.png",
        "sourceUrl": REPO,
        "rootRequired": False,
        "ipkUrl": f"{REPO}/releases/download/{tag}/{os.path.basename(ipk)}",
        "ipkHash": {"sha256": hashlib.sha256(data).hexdigest()},
    }
    with open(out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(json.dumps(manifest, indent=2))


main()
