#!/usr/bin/env python3
"""Build a source-only DKMS deb from the pinned Ubuntu kernel source package."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile

VERSION = "0.1.0"
SOURCE_VERSION = "7.0.0-38.38"
SOURCE_SHA256 = "997a13a0c7fd0172a93b0c1ae9a4ffc7d3cbbf91d2fdd91d85334193580687de"
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_deb", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source_deb = args.source_deb.resolve()
    with source_deb.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != SOURCE_SHA256:
        parser.error(f"Expected Ubuntu linux-source-7.0.0 {SOURCE_VERSION}; SHA256 mismatch")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rubikpi3-dkms-") as work:
        work = Path(work)
        subprocess.run(["dpkg-deb", "-x", str(source_deb), str(work / "ubuntu")], check=True)
        package = work / "package"
        src = package / "usr/src" / f"rubikpi3-msm-{VERSION}"
        src.mkdir(parents=True)
        archive = work / "ubuntu/usr/src/linux-source-7.0.0.tar.bz2"
        # Copy only regular files from the pinned archive, preserving the DRM
        # hierarchy: MSM includes internal headers from its parent directory.
        with tarfile.open(archive, "r:bz2") as tar:
            for member in tar:
                parts = PurePosixPath(member.name).parts
                if not member.isfile() or parts[0] != "linux-source-7.0.0":
                    continue
                rel = PurePosixPath(*parts[1:])
                if ".." in rel.parts:
                    raise ValueError(f"Unsafe source path: {member.name}")
                name = str(rel)
                selected = (
                    name.startswith("drivers/gpu/drm/msm/")
                    or (str(rel.parent) == "drivers/gpu/drm" and rel.suffix == ".h")
                    or name.startswith("LICENSES/")
                    or name == "COPYING"
                )
                if selected:
                    dest = src / name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(member) as incoming, dest.open("wb") as outgoing:
                        shutil.copyfileobj(incoming, outgoing)
        for patch in sorted((HERE / "patches").glob("*.patch")):
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)], cwd=src, check=True)
        for name in ("dkms.conf", "refresh-initramfs"):
            shutil.copy2(HERE / "packaging" / name, src / name)
        (src / "refresh-initramfs").chmod(0o755)
        provenance = f"Ubuntu linux-source-7.0.0 {SOURCE_VERSION}\nSHA256 {SOURCE_SHA256}\n"
        (src / "SOURCE").write_text(provenance)
        shutil.copytree(HERE / "patches", src / "applied-patches")
        docs = package / "usr/share/doc/rubikpi3-msm-dkms"
        docs.mkdir(parents=True)
        for name in ("README.md", "README.en.md", "VALIDATION.md"):
            shutil.copy2(HERE / name, docs / name)
        shutil.copy2(src / "COPYING", docs / "copyright")
        conf = package / "usr/lib/modprobe.d"
        conf.mkdir(parents=True)
        shutil.copy2(HERE / "packaging/rubikpi3-msm-dkms.conf", conf)
        debian = package / "DEBIAN"
        debian.mkdir()
        installed_size = (sum(p.stat().st_size for p in package.rglob("*") if p.is_file()) + 1023) // 1024
        (debian / "control").write_text(f"""Package: rubikpi3-msm-dkms
Version: {VERSION}
Section: kernel
Priority: optional
Architecture: arm64
Installed-Size: {installed_size}
Maintainer: flange-build <flange-build@users.noreply.github.com>
Depends: dkms (>= 3.2), build-essential, python3 (>= 3.11), kmod, initramfs-tools | dracut
Recommends: linux-headers-generic
Conflicts: kgsl-dkms
Description: RubikPi3 EL2 Adreno MSM driver for Ubuntu 7.0 generic
 Ubuntu MSM driver with a RubikPi3-only VHE/EL2 direct SECVID workaround.
 Source is based on Ubuntu {SOURCE_VERSION}. Other kernel series are excluded.
 Uses Mesa Freedreno/Turnip and keeps UEFI simpledrm scanout.
""")
        for name in ("postinst", "prerm", "postrm"):
            shutil.copy2(HERE / "packaging" / name, debian / name)
            (debian / name).chmod(0o755)
        checksums = []
        for path in sorted(package.rglob("*")):
            if path.is_dir():
                path.chmod(0o755)
            elif path.is_file():
                executable = path.name == "refresh-initramfs" or path.parent == debian and path.name in ("postinst", "prerm", "postrm")
                path.chmod(0o755 if executable else 0o644)
                if debian not in path.parents:
                    checksums.append(f"{hashlib.md5(path.read_bytes()).hexdigest()}  {path.relative_to(package)}\n")
        (debian / "md5sums").write_text("".join(checksums))
        (debian / "md5sums").chmod(0o644)
        package.chmod(0o755)
        deb = output / f"rubikpi3-msm-dkms_{VERSION}_arm64.deb"
        subprocess.run(["dpkg-deb", "--root-owner-group", "-Zxz", "--build", str(package), str(deb)], check=True)
        print(deb)


if __name__ == "__main__":
    main()
