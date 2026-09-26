#!/usr/bin/env python3
"""package-release -- build, verify and compress a crt-bridge emitter release.

Three subcommands, all stdlib-only (no RetroArch launched, nothing built):

    package-release.py stage --platform {windows,linux} --fork-dir DIR \\
        --libgm-dir DIR --binary PATH --version VER --fork-commit SHA40 \\
        --libgm-commit SHA40 --out DIR [--libgm-dll PATH] \\
        [--mingw-bin DIR] [--msys-root DIR] [--objdump PATH]
        Stages an archive folder from an explicit allowlist: the renamed
        binary, the platform crt-bridge.cfg template (target left empty),
        a launcher, a substituted README, the "mister" autoconfig profile
        (autoconfig/mister/MiSTer.cfg, so a release user's controller
        works without hand-editing a config file), LICENSES/ (measured,
        never a recopied list), and an empty cores/ marker. Nothing else
        goes in.

    package-release.py check --platform {windows,linux} \\
        (--dir DIR | --archive FILE) [--mingw-bin DIR] [--objdump PATH] \\
        [--require-branding]
        Verifies a staged directory or a built archive against the same
        rules a public CI run must pass before Sir ever tags a release:
        empty bridge target, no private address/host path, no core, no
        official branding in file names, every license present, the
        autoconfig profile with its axis binds, and (on Windows, with
        --mingw-bin) a closed DLL import set.

    package-release.py archive --platform {windows,linux} --dir DIR --out FILE
        Compresses a staged directory: .zip on Windows, .tar.gz on Linux
        (root-owned, 0755 on the executable and the shell launcher, 0644
        elsewhere).

Exit codes: 0 = PASS / success, 1 = findings (check only), 2 = usage or
environment error (bad version string, missing required input, output
already exists).
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Optional, Sequence

# ---------------------------------------------------------------------------
# Shared regexes and parsers
# ---------------------------------------------------------------------------

# Version tag: upstream base + crt-bridge counter, e.g. v1.22.2-crtbridge.1.
VERSION_RE = re.compile(r"^v(\d+\.\d+\.\d+)-crtbridge\.([1-9]\d*)$")
SHA40_RE = re.compile(r"^[0-9a-fA-F]{40}$")

# RetroArch .cfg line format: `key = "value"` or `key = value` (no [section]
# headers -- configparser does not apply). Same pattern as
# emitter/test/test_config_preset.py::KEY_RE in the private repo.
KEY_RE = re.compile(r'^\s*([A-Za-z0-9_]+)\s*=\s*"?([^"\n]*?)"?\s*$')

# Recopied from scripts/guard-fork-publish.py (PRIVATE_IPV4_RE, MAC_RE,
# _MAC_DOC_PREFIX) -- this file ships in the PUBLIC fork and runs in public
# CI, which never sees the private crt-bridge repo, so importing the guard
# is not an option; the rules must be duplicated here instead.
_OCTET = r"(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])"
PRIVATE_IPV4_RE = re.compile(
    (
        r"\b(?:"
        r"10\.{o}\.{o}\.{o}"
        r"|172\.(?:1[6-9]|2[0-9]|3[01])\.{o}\.{o}"
        r"|192\.168\.{o}\.{o}"
        r")\b"
    ).format(o=_OCTET)
)
MAC_RE = re.compile(r"\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b")
_MAC_DOC_PREFIX = "00:00:5e:00:53"  # RFC 7042 documentation range, not a finding.

# Machine-specific home/user paths -- not covered by the guard's IP/MAC
# rules, but a path naming someone's own machine is just as disqualifying
# for a release template as an address.
MACHINE_PATH_RE = re.compile(r"(?i)\b[a-z]:\\users\\|/home/[^/\s]+|/Users/[^/\s]+|~[\\/]")

TOKEN_RE = re.compile(r"@[A-Z_]+@")


def _is_doc_mac(matched: str) -> bool:
    return matched.replace("-", ":").lower().startswith(_MAC_DOC_PREFIX)


def parse_cfg(path: Path) -> dict:
    out: dict = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.split("#", 1)[0].strip()
        if not stripped:
            continue
        m = KEY_RE.match(stripped)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def _scan_leak(text: str) -> list[str]:
    """Return a list of human-readable leak descriptions found in `text`."""
    findings: list[str] = []
    for m in PRIVATE_IPV4_RE.finditer(text):
        findings.append(f"private IPv4 address {m.group(0)!r}")
    for m in MAC_RE.finditer(text):
        if _is_doc_mac(m.group(0)):
            continue
        findings.append(f"MAC address {m.group(0)!r}")
    for m in MACHINE_PATH_RE.finditer(text):
        findings.append(f"machine-specific path {m.group(0)!r}")
    return findings


# ---------------------------------------------------------------------------
# DLL import-closure walk (Windows) -- objdump -p, never a recopied list.
# Same algorithm as emitter/retroarch/build/build-retroarch-fork.sh, step
# 7c, rewritten here since this file must stand alone in the public fork.
# ---------------------------------------------------------------------------


def _dll_imports(path: Path, objdump: str) -> list[str]:
    result = subprocess.run([objdump, "-p", str(path)], capture_output=True, text=True)
    names: list[str] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("DLL Name:"):
            names.append(line.split(":", 1)[1].strip())
    return names


def _find_in_dir_ci(directory: Path, name: str) -> Optional[Path]:
    if not directory.is_dir():
        return None
    lname = name.lower()
    for entry in directory.iterdir():
        if entry.is_file() and entry.name.lower() == lname:
            return entry
    return None


def dll_closure(
    start_paths: Sequence[Path], mingw_bin: Optional[Path], objdump: str
) -> dict:
    """Walk the transitive import closure of `start_paths`.

    Returns {"seen": set(all DLL names encountered), "copied":
    {name: source_path} for every name found in `mingw_bin`}. Anything not
    found in `mingw_bin` is treated as a system DLL and left alone -- this
    mirrors build-retroarch-fork.sh exactly.
    """
    seen: set = set()
    copied: dict = {}
    queue: list = []
    for p in start_paths:
        queue.extend(_dll_imports(p, objdump))
    while queue:
        nxt: list = []
        for name in queue:
            if name in seen:
                continue
            seen.add(name)
            if mingw_bin is None:
                continue
            src = _find_in_dir_ci(mingw_bin, name)
            if src is None:
                continue
            copied[name] = src
            nxt.extend(_dll_imports(src, objdump))
        queue = nxt
    return {"seen": seen, "copied": copied}


# ---------------------------------------------------------------------------
# MSYS2/pacman license discovery -- measured from the local package
# database, never a hand-copied list.
# ---------------------------------------------------------------------------


def _pkg_name_without_version(pkg_dir_name: str) -> str:
    """`<name>-<pkgver>-<pkgrel>` -> `<name>` (pacman local/ dir convention;
    pkgver/pkgrel never contain hyphens, so splitting the last two hyphen
    groups off the right recovers the bare package name)."""
    parts = pkg_dir_name.rsplit("-", 2)
    return parts[0] if len(parts) == 3 else pkg_dir_name


def _find_owning_package(msys_root: Path, dll_name: str) -> Optional[Path]:
    local = msys_root / "var/lib/pacman/local"
    if not local.is_dir():
        return None
    target = f"mingw64/bin/{dll_name}".lower()
    for pkg_dir in sorted(local.iterdir()):
        files = pkg_dir / "files"
        if not files.is_file():
            continue
        for line in files.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip().lower() == target:
                return pkg_dir
    return None


def copy_mingw_runtime_licenses(
    msys_root: Path, pkg_dir: Path, dest_root: Path
) -> int:
    """Copy every `mingw64/share/licenses/...` entry owned by `pkg_dir`
    into `dest_root/<pkgname-without-version>/...`, preserving the
    sub-tree under `share/licenses/`. Returns the number of files copied.
    """
    files = pkg_dir / "files"
    pkg_name = _pkg_name_without_version(pkg_dir.name)
    dest_pkg = dest_root / pkg_name
    prefix = "mingw64/share/licenses/"
    count = 0
    for line in files.read_text(encoding="utf-8", errors="replace").splitlines():
        rel = line.strip()
        if not rel.startswith(prefix) or rel.endswith("/"):
            continue
        src = msys_root / rel
        if not src.is_file():
            continue
        dst = dest_pkg / rel[len(prefix):]
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        count += 1
    return count


# ---------------------------------------------------------------------------
# stage
# ---------------------------------------------------------------------------


def cmd_stage(args: argparse.Namespace) -> int:
    m = VERSION_RE.match(args.version)
    if not m:
        print(
            f"ERROR: --version {args.version!r} does not match "
            "v<base>-crtbridge.<N> (e.g. v1.22.2-crtbridge.1)",
            file=sys.stderr,
        )
        return 2
    base_version = m.group(1)

    if not SHA40_RE.match(args.fork_commit):
        print(f"ERROR: --fork-commit {args.fork_commit!r} is not 40 hex chars", file=sys.stderr)
        return 2
    if not SHA40_RE.match(args.libgm_commit):
        print(f"ERROR: --libgm-commit {args.libgm_commit!r} is not 40 hex chars", file=sys.stderr)
        return 2

    fork_dir = Path(args.fork_dir)
    libgm_dir = Path(args.libgm_dir)
    binary_src = Path(args.binary)
    out_root = Path(args.out)

    stage_name = f"crt-bridge-emitter-{args.version}-{args.platform}-x64"
    stage_dir = out_root / stage_name
    if stage_dir.exists():
        print(f"ERROR: staging directory already exists: {stage_dir}", file=sys.stderr)
        return 2
    stage_dir.mkdir(parents=True)

    staged: list[str] = []

    def _stage_file(src: Path, rel: str, *, executable: bool = False) -> Path:
        dst = stage_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        if executable:
            dst.chmod(0o755)
        staged.append(rel)
        return dst

    # -- Binary, renamed --
    binary_name = "crt-bridge-emitter.exe" if args.platform == "windows" else "crt-bridge-emitter"
    if not binary_src.is_file():
        print(f"ERROR: --binary not found: {binary_src}", file=sys.stderr)
        return 2
    binary_dst = _stage_file(binary_src, binary_name, executable=(args.platform == "linux"))

    # -- crt-bridge.cfg, byte-for-byte from the fork's own template --
    cfg_src = fork_dir / "pkg" / "crt-bridge" / f"crt-bridge-{args.platform}.cfg"
    if not cfg_src.is_file():
        print(f"ERROR: config template not found: {cfg_src}", file=sys.stderr)
        return 2
    _stage_file(cfg_src, "crt-bridge.cfg")

    # -- Autoconfig profile: the "mister" driver's autoconfig cfg, so a
    # release user's controller works without hand-editing a config file.
    # Shipped for both platforms -- the driver's joypad half is built on
    # both (crt-bridge-linux.cfg keeps input_joypad_driver = "mister"). --
    autoconfig_src = fork_dir / "pkg" / "crt-bridge" / "autoconfig" / "mister" / "MiSTer.cfg"
    if not autoconfig_src.is_file():
        print(f"ERROR: autoconfig profile not found: {autoconfig_src}", file=sys.stderr)
        return 2
    _stage_file(autoconfig_src, "autoconfig/mister/MiSTer.cfg")

    # -- Launcher --
    if args.platform == "windows":
        launcher_src = fork_dir / "pkg" / "crt-bridge" / "start-emitter.cmd"
        _stage_file(launcher_src, "start-emitter.cmd")
    else:
        launcher_src = fork_dir / "pkg" / "crt-bridge" / "start-emitter.sh"
        _stage_file(launcher_src, "start-emitter.sh", executable=True)

    # -- README.md, substituted from the gabarit --
    readme_src = fork_dir / "pkg" / "crt-bridge" / "README-release.md"
    if not readme_src.is_file():
        print(f"ERROR: README template not found: {readme_src}", file=sys.stderr)
        return 2
    platform_label = "Windows x64" if args.platform == "windows" else "Linux x64"
    readme_text = readme_src.read_text(encoding="utf-8")
    readme_text = (
        readme_text.replace("@VERSION@", args.version)
        .replace("@BASE_VERSION@", base_version)
        .replace("@PLATFORM@", platform_label)
        .replace("@FORK_COMMIT@", args.fork_commit)
        .replace("@LIBGM_COMMIT@", args.libgm_commit)
    )
    readme_dst = stage_dir / "README.md"
    readme_dst.write_text(readme_text, encoding="utf-8")
    staged.append("README.md")

    # -- cores/ marker, no core shipped --
    cores_dir = stage_dir / "cores"
    cores_dir.mkdir()
    (cores_dir / "PUT-CORES-HERE.txt").write_text(
        "Put your libretro cores here. No core is shipped with crt-bridge emitter.\n",
        encoding="utf-8",
    )
    staged.append("cores/PUT-CORES-HERE.txt")

    # -- LICENSES/ --
    licenses_dir = stage_dir / "LICENSES"
    licenses_dir.mkdir()

    notice_src = fork_dir / "pkg" / "crt-bridge" / "NOTICE.txt"
    if not notice_src.is_file():
        print(f"ERROR: NOTICE.txt template not found: {notice_src}", file=sys.stderr)
        return 2
    _stage_file(notice_src, "LICENSES/NOTICE.txt")

    copying_src = fork_dir / "COPYING"
    if not copying_src.is_file():
        print(f"ERROR: fork COPYING not found: {copying_src}", file=sys.stderr)
        return 2
    _stage_file(copying_src, "LICENSES/emitter-GPL-3.0.txt")

    libgm_license_src = libgm_dir / "LICENSE"
    if not libgm_license_src.is_file():
        print(f"ERROR: libgm LICENSE not found: {libgm_license_src}", file=sys.stderr)
        return 2
    _stage_file(libgm_license_src, "LICENSES/libgm-GPL-3.0-or-later.txt")

    lz4_header = libgm_dir / "src" / "lz4.h"
    if not lz4_header.is_file():
        print(f"ERROR: {lz4_header} not found", file=sys.stderr)
        return 2
    lz4_text = lz4_header.read_text(encoding="utf-8", errors="replace")
    block_match = re.search(r"/\*.*?\*/", lz4_text, re.DOTALL)
    if not block_match or "BSD 2-Clause License" not in block_match.group(0):
        print(
            f"ERROR: {lz4_header} does not open with a BSD 2-Clause License comment block",
            file=sys.stderr,
        )
        return 2
    lz4_dst = licenses_dir / "lz4-BSD-2-Clause.txt"
    lz4_dst.write_text(block_match.group(0), encoding="utf-8")
    staged.append("LICENSES/lz4-BSD-2-Clause.txt")

    # -- Windows only: libgm.dll + measured DLL closure + mingw licenses --
    if args.platform == "windows":
        libgm_dll_src = Path(args.libgm_dll) if args.libgm_dll else (libgm_dir / "libgm.dll")
        if not libgm_dll_src.is_file():
            print(f"ERROR: libgm.dll not found: {libgm_dll_src}", file=sys.stderr)
            return 2
        libgm_dll_dst = _stage_file(libgm_dll_src, "libgm.dll")

        mingw_bin = Path(args.mingw_bin) if args.mingw_bin else None
        objdump = args.objdump or "objdump"
        closure = dll_closure([binary_dst, libgm_dll_dst], mingw_bin, objdump)

        if closure["copied"] and args.msys_root is None:
            print(
                "ERROR: mingw DLLs were found in the import closure but --msys-root "
                "was not given -- cannot locate their licenses",
                file=sys.stderr,
            )
            return 2

        msys_root = Path(args.msys_root) if args.msys_root else None
        for name, src in sorted(closure["copied"].items()):
            dst = stage_dir / name
            shutil.copyfile(src, dst)
            staged.append(name)

            pkg_dir = _find_owning_package(msys_root, name) if msys_root else None
            if pkg_dir is None:
                print(
                    f"ERROR: no MSYS2 package owns mingw64/bin/{name} under {msys_root} "
                    "-- cannot attach a measured license",
                    file=sys.stderr,
                )
                return 2
            n = copy_mingw_runtime_licenses(msys_root, pkg_dir, licenses_dir / "mingw-runtime")
            if n == 0:
                print(
                    f"ERROR: package {pkg_dir.name!r} (owner of {name}) ships no "
                    "mingw64/share/licenses/ files",
                    file=sys.stderr,
                )
                return 2

    for rel in sorted(staged):
        print(f"staged: {rel}")
    return 0


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------


def _iter_members(root: Path) -> list[Path]:
    return [p for p in root.rglob("*") if p.is_file()]


def _rule_cfg_target(root: Path) -> list[tuple[str, str]]:
    cfg = root / "crt-bridge.cfg"
    if not cfg.is_file():
        return [("cfg-target", str(cfg) + ": missing crt-bridge.cfg")]
    parsed = parse_cfg(cfg)
    findings = []
    for key in ("video_record_config", "groovy_followers"):
        if key not in parsed:
            findings.append(("cfg-target", f"{cfg}: {key} is missing"))
        elif parsed[key] != "":
            findings.append(("cfg-target", f"{cfg}: {key} = {parsed[key]!r}, expected empty"))
    return findings


def _rule_private_address(root: Path) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    candidates: list[Path] = []
    for rel in ("crt-bridge.cfg", "README.md"):
        p = root / rel
        if p.is_file():
            candidates.append(p)
    candidates.extend(sorted(root.glob("start-emitter.*")))
    notice = root / "LICENSES" / "NOTICE.txt"
    if notice.is_file():
        candidates.append(notice)
    cores = root / "cores"
    if cores.is_dir():
        candidates.extend(sorted(cores.glob("*.txt")))
    for path in candidates:
        text = path.read_text(encoding="utf-8", errors="replace")
        for leak in _scan_leak(text):
            findings.append(("private-address", f"{path}: {leak}"))
    return findings


_AUTOCONFIG_AXIS_KEYS = (
    "input_l_x_plus_axis",
    "input_l_x_minus_axis",
    "input_l_y_plus_axis",
    "input_l_y_minus_axis",
    "input_r_x_plus_axis",
    "input_r_x_minus_axis",
    "input_r_y_plus_axis",
    "input_r_y_minus_axis",
)


def _rule_autoconfig_profile(root: Path) -> list[tuple[str, str]]:
    profile = root / "autoconfig" / "mister" / "MiSTer.cfg"
    if not profile.is_file():
        return [("autoconfig-profile", f"{profile}: missing")]
    parsed = parse_cfg(profile)
    findings: list[tuple[str, str]] = []
    if parsed.get("input_driver") != "mister":
        findings.append(
            (
                "autoconfig-profile",
                f"{profile}: input_driver = {parsed.get('input_driver')!r}, expected 'mister'",
            )
        )
    for key in _AUTOCONFIG_AXIS_KEYS:
        if key not in parsed:
            findings.append(("autoconfig-profile", f"{profile}: missing {key}"))
    return findings


def _rule_core(root: Path, platform: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    core_name_re = re.compile(r"(?i)_libretro\.(dll|so|dylib)$")
    for member in _iter_members(root):
        if core_name_re.search(member.name):
            findings.append(("core", f"{member}: looks like a libretro core"))
        if platform == "linux" and member.suffix == ".so":
            findings.append(("core", f"{member}: a .so is present in a Linux archive"))
    cores_dir = root / "cores"
    if cores_dir.is_dir():
        extra = sorted(
            str(p.relative_to(cores_dir)) for p in cores_dir.rglob("*")
            if p.is_file() and p.name != "PUT-CORES-HERE.txt"
        )
        for rel in extra:
            findings.append(("core", f"{cores_dir / rel}: unexpected file in cores/"))
    return findings


def _rule_name(root: Path, platform: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    for member in _iter_members(root):
        rel = str(member.relative_to(root))
        if "retroarch" in rel.lower():
            findings.append(("name", f"{member}: path contains 'retroarch'"))
    expected = "crt-bridge-emitter.exe" if platform == "windows" else "crt-bridge-emitter"
    if not (root / expected).is_file():
        findings.append(("name", f"{root / expected}: executable not found under expected name"))
    return findings


def _rule_branding(root: Path, platform: str) -> list[tuple[str, str]]:
    expected = "crt-bridge-emitter.exe" if platform == "windows" else "crt-bridge-emitter"
    binary = root / expected
    if not binary.is_file():
        return [("branding", f"{binary}: missing, cannot check branding")]
    data = binary.read_bytes()
    if b"crt-bridge emitter" not in data:
        return [("branding", f"{binary}: 'crt-bridge emitter' not found in binary bytes")]
    return []


def _rule_readme(root: Path) -> list[tuple[str, str]]:
    readme = root / "README.md"
    if not readme.is_file():
        return [("readme", f"{readme}: missing")]
    text = readme.read_text(encoding="utf-8", errors="replace")
    findings: list[tuple[str, str]] = []
    if "Based on RetroArch" not in text:
        findings.append(("readme", f"{readme}: missing 'Based on RetroArch'"))
    if not re.search(r"https://github\.com/crt-bridge/libgm/tree/[0-9a-fA-F]{40}", text):
        findings.append(("readme", f"{readme}: missing a pinned libgm source URL"))
    leftover = TOKEN_RE.findall(text)
    if leftover:
        findings.append(("readme", f"{readme}: unsubstituted token(s) {sorted(set(leftover))}"))
    return findings


def _rule_licenses(root: Path, platform: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    licenses_dir = root / "LICENSES"
    required = [
        "NOTICE.txt",
        "emitter-GPL-3.0.txt",
        "libgm-GPL-3.0-or-later.txt",
        "lz4-BSD-2-Clause.txt",
    ]
    for name in required:
        p = licenses_dir / name
        if not p.is_file() or p.stat().st_size == 0:
            findings.append(("licenses", f"{p}: missing or empty"))
    if platform == "windows":
        mingw_dir = licenses_dir / "mingw-runtime"
        if not mingw_dir.is_dir() or not any(mingw_dir.rglob("*")):
            findings.append(("licenses", f"{mingw_dir}: missing or empty"))
    return findings


def _rule_dll_closure(root: Path, mingw_bin: Path, objdump: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    binary = root / "crt-bridge-emitter.exe"
    dlls = sorted(root.glob("*.dll"))
    targets = ([binary] if binary.is_file() else []) + dlls
    present = {p.name.lower() for p in dlls}
    for target in targets:
        for imp in _dll_imports(target, objdump):
            if _find_in_dir_ci(mingw_bin, imp) is not None and imp.lower() not in present:
                findings.append(
                    ("dll-closure", f"{target}: imports {imp!r} (a mingw DLL) not present in the archive")
                )
    if binary.is_file():
        imports = {n.lower() for n in _dll_imports(binary, objdump)}
        if "libgm.dll" not in imports:
            findings.append(("dll-closure", f"{binary}: does not import libgm.dll"))
    return findings


def cmd_check(args: argparse.Namespace) -> int:
    tmp_holder: Optional[tempfile.TemporaryDirectory] = None
    if args.archive:
        archive_path = Path(args.archive)
        tmp_holder = tempfile.TemporaryDirectory(prefix="crt-bridge-check-")
        extract_root = Path(tmp_holder.name)
        if args.platform == "windows":
            with zipfile.ZipFile(archive_path) as zf:
                zf.extractall(extract_root)
        else:
            with tarfile.open(archive_path, "r:gz") as tf:
                tf.extractall(extract_root)
        entries = list(extract_root.iterdir())
        if len(entries) == 1 and entries[0].is_dir():
            root = entries[0]
        else:
            root = extract_root
    elif args.dir:
        root = Path(args.dir)
    else:
        print("ERROR: one of --dir or --archive is required", file=sys.stderr)
        return 2

    findings: list[tuple[str, str]] = []
    findings.extend(_rule_cfg_target(root))
    findings.extend(_rule_private_address(root))
    findings.extend(_rule_autoconfig_profile(root))
    findings.extend(_rule_core(root, args.platform))
    findings.extend(_rule_name(root, args.platform))
    if args.require_branding:
        findings.extend(_rule_branding(root, args.platform))
    findings.extend(_rule_readme(root))
    findings.extend(_rule_licenses(root, args.platform))
    if args.platform == "windows" and args.mingw_bin:
        findings.extend(
            _rule_dll_closure(root, Path(args.mingw_bin), args.objdump or "objdump")
        )

    for rule, detail in findings:
        print(f"FAIL {rule} {detail}")
    if tmp_holder is not None:
        tmp_holder.cleanup()
    if findings:
        print(f"FAIL ({len(findings)})")
        return 1
    print("PASS")
    return 0


# ---------------------------------------------------------------------------
# archive
# ---------------------------------------------------------------------------


def cmd_archive(args: argparse.Namespace) -> int:
    src_dir = Path(args.dir)
    if not src_dir.is_dir():
        print(f"ERROR: --dir not found: {src_dir}", file=sys.stderr)
        return 2
    out_path = Path(args.out)
    if out_path.exists():
        print(f"ERROR: --out already exists: {out_path}", file=sys.stderr)
        return 2

    root_name = src_dir.name
    files = sorted(p for p in src_dir.rglob("*") if p.is_file())

    if args.platform == "windows":
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in files:
                arcname = f"{root_name}/{f.relative_to(src_dir).as_posix()}"
                zf.write(f, arcname)
    else:
        executables = {"crt-bridge-emitter", "start-emitter.sh"}

        def _filter(tarinfo: tarfile.TarInfo) -> tarfile.TarInfo:
            tarinfo.uid = 0
            tarinfo.gid = 0
            tarinfo.uname = "root"
            tarinfo.gname = "root"
            base = Path(tarinfo.name).name
            tarinfo.mode = 0o755 if base in executables else 0o644
            return tarinfo

        with tarfile.open(out_path, "w:gz") as tf:
            for f in files:
                arcname = f"{root_name}/{f.relative_to(src_dir).as_posix()}"
                tf.add(f, arcname=arcname, filter=_filter)

    print(f"archived: {out_path}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(prog="package-release.py", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_stage = sub.add_parser("stage")
    p_stage.add_argument("--platform", required=True, choices=["windows", "linux"])
    p_stage.add_argument("--fork-dir", required=True)
    p_stage.add_argument("--libgm-dir", required=True)
    p_stage.add_argument("--binary", required=True)
    p_stage.add_argument("--version", required=True)
    p_stage.add_argument("--fork-commit", required=True)
    p_stage.add_argument("--libgm-commit", required=True)
    p_stage.add_argument("--out", required=True)
    p_stage.add_argument("--libgm-dll")
    p_stage.add_argument("--mingw-bin")
    p_stage.add_argument("--msys-root")
    p_stage.add_argument("--objdump")

    p_check = sub.add_parser("check")
    p_check.add_argument("--platform", required=True, choices=["windows", "linux"])
    p_check.add_argument("--dir")
    p_check.add_argument("--archive")
    p_check.add_argument("--mingw-bin")
    p_check.add_argument("--objdump")
    p_check.add_argument("--require-branding", action="store_true")

    p_archive = sub.add_parser("archive")
    p_archive.add_argument("--platform", required=True, choices=["windows", "linux"])
    p_archive.add_argument("--dir", required=True)
    p_archive.add_argument("--out", required=True)

    args = parser.parse_args(argv)

    if args.command == "stage":
        return cmd_stage(args)
    if args.command == "check":
        return cmd_check(args)
    if args.command == "archive":
        return cmd_archive(args)
    return 2  # unreachable -- argparse subparsers are required


if __name__ == "__main__":
    raise SystemExit(main())
