#!/usr/bin/env python3
"""package-release -- build, verify and compress a crt-bridge emitter release.

Five subcommands, all stdlib-only (no RetroArch launched, nothing built):

    package-release.py stage --platform {windows,linux} --fork-dir DIR \\
        --libgm-dir DIR --binary PATH --version VER --fork-commit SHA40 \\
        --libgm-commit SHA40 --out DIR [--libgm-dll PATH] \\
        [--mingw-bin DIR] [--msys-root DIR] [--objdump PATH]
        Stages an archive folder from an explicit allowlist: the renamed
        binary, the platform crt-bridge.cfg template (target left empty),
        a launcher, a substituted README, the "mister" autoconfig profile
        (autoconfig/mister/MiSTer.cfg, so a release user's controller
        works without hand-editing a config file), LICENSES/ (the texts of
        the COMPONENTS table copied from the fork, never typed here, and
        the licenses of the MinGW runtime DLLs measured from the MSYS2
        package database), and an empty cores/ marker. Nothing else goes
        in.

    package-release.py check --platform {windows,linux} \\
        (--dir DIR | --archive FILE) [--mingw-bin DIR] [--objdump PATH] \\
        [--require-branding] [--forbid-bench-strings]
        Verifies a staged directory or a built archive against the same
        rules a public CI run must pass before Sir ever tags a release:
        empty bridge target, no private address/host path, no core, no
        official branding in file names, every license present, every
        `Text:` line of LICENSES/NOTICE.txt backed by a non-empty file and
        every file of LICENSES/ named by one, the autoconfig profile with
        its axis binds, and (on Windows, with --mingw-bin) a closed DLL
        import set. With --forbid-bench-strings,
        the executable and every DLL of the kit must also be free of the
        bench-only strings (BENCH_STRINGS) and of private addresses and
        personal paths; this rule is opt-in because a bench binary staged
        into a kit for a test legitimately carries them.

    package-release.py check-bench-strings --binary FILE [--dll FILE ...]
        Looks for every BENCH_STRINGS token (as ASCII and as UTF-16LE) and
        for private addresses and personal paths in the given files. Any
        file is accepted, an object file included. A leak is reported by
        rule and position only, never with the string found.

    package-release.py check-notice-objects --obj-dir DIR \\
        --platform {windows,linux}
        Reads the object directory of a finished build (obj-unix/release)
        and fails when a third-party object folder (deps/*, libchdr) has
        no line in the COMPONENTS table, when a line is seen in a build of
        the other platform, or when an object that includes a header-only
        component is there while its line is not. The CI runs it after
        "Build": the build itself proves the notice lists what it compiled.

    package-release.py archive --platform {windows,linux} --dir DIR --out FILE
        Compresses a staged directory: .zip on Windows, .tar.gz on Linux
        (root-owned, 0755 on the executable and the shell launcher, 0644
        elsewhere).

Exit codes: 0 = PASS / success, 1 = findings (check, check-bench-strings and
check-notice-objects only), 2 = usage or environment error (bad version
string, missing or unreadable required input, output already exists).
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import NamedTuple, Optional, Sequence

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

# Bench-only strings: environment variable names and log markers of the
# diagnostics that exist only in a build made with HAVE_GROOVY_BENCH=1. This
# is the ONE list: it is read by `check-bench-strings`, by `check
# --forbid-bench-strings`, by the CI step that calls the first, and by the
# tests. A token leaves the list only with a written reason. A token is a
# plain substring, searched in the raw bytes of a file, so it is found
# whatever the linker did with the string table.
#   GROOVY_DUMP_FRAME covers its _PATH, _INTERVAL and _SEQ variants;
#   GROOVY_REANNOUNCE covers its _SPACING_US variant.
# Names that stay public must never match a token: GROOVY_FRAME_DUP,
# GROOVY_SERVO, GROOVY_AUDIO_RATIO, GROOVY_USO, GROOVY_FIELD_PER_FRAME,
# GROOVY_FOLLOWERS, GROOVY_MTU, GROOVY_PAD, GROOVY_DOWNSAMPLE, GROOVY_31KHZ,
# [groovy-mode], [groovy-annonce], [groovy-emit], [groovy-input], ack_lost,
# "[groovy-servo] settings:" and "[groovy-servo] feed-forward:".
BENCH_STRINGS = (
    # environment variables
    "GROOVY_INJECT",
    "GROOVY_FRAMETIME",
    "GROOVY_SKIP_FRAME_EVERY",
    "GROOVY_SERVO_TRACE",
    "GROOVY_DUMP_FRAME",
    "GROOVY_VK_FLIP",
    "GROOVY_D3D_FLIP",
    "GROOVY_AUDIO_HOLD",
    "GROOVY_REANNOUNCE",
    # log markers and lines
    "[groovy-inject]",
    "[groovy-frametime]",
    "[groovy-conv]",
    "[groovy-servo-trace]",
    "[groovy-lat]",
    "[groovy-input-lat]",
    "[groovy-clock]",
    "[groovy-diag]",
    "[groovy-box]",
    "[groovy-audio-hold]",
    "[groovy-servo] state=",
    "[groovy] skip: one frame in",
    "push_video vid:",
    # bench lines with no marker of their own: each has its own token, so a
    # line that leaves its gated block is seen even when the variable read
    # next to it stays gated (the public "[groovy] re-announce: on|off"
    # line must not match)
    "frame witness active:",
    "re-announce: spacing",
    # counter (latency line field, XAudio2 underrun counter)
    "pc_underruns",
)

# A personal path inside a binary: a drive-letter or POSIX home directory
# followed by a user-name segment. The bare `~/` form of MACHINE_PATH_RE is
# left out on purpose: it is ordinary text in a binary.
BINARY_HOME_PATH_RE = re.compile(r"(?i)\b[a-z]:[\\/]users[\\/][^\\/\s]+|/home/[^/\s]+|/Users/[^/\s]+")

# Printable runs of a binary: ASCII, and UTF-16LE (a printable byte followed
# by a zero byte), five characters at least.
_ASCII_RUN_RE = re.compile(rb"[\x20-\x7e]{5,}")
_UTF16_RUN_RE = re.compile(rb"(?:[\x20-\x7e]\x00){5,}")

# Every RetroArch directory-config key, relative to the kit. Read off
# configuration.c's SETTING_PATH/SETTING_ARRAY calls on this branch. Left
# out, with the reason:
#   - rgui_browser_directory: the file browser's start folder, never
#     written to -- fixing it would hide the user's own folders.
#   - any key gated behind a platform #if this kit does not target (e.g.
#     bottom_assets_directory, 3DS-only).
#   - file keys (core_options_path, content_*_path, video_font_path,
#     audio_dsp_plugin, video_filter, menu_wallpaper, ...): their default
#     derives from the directories below, or from the config file's own
#     folder.
KIT_DIR_KEYS = {
    "libretro_directory": "cores",
    "libretro_info_path": "info",
    "joypad_autoconfig_dir": "autoconfig",
    "system_directory": "system",
    "savefile_directory": "saves",
    "savestate_directory": "states",
    "screenshot_directory": "screenshots",
    "playlist_directory": "playlists",
    "content_favorites_directory": "playlists",
    "content_history_directory": "playlists",
    "content_image_history_directory": "playlists",
    "content_music_history_directory": "playlists",
    "content_video_directory": "playlists",
    "cache_directory": "cache",
    "log_dir": "logs",
    "runtime_log_directory": "logs/runtime",
    "rgui_config_directory": "config",
    "input_remapping_directory": "config/remaps",
    "recording_output_directory": "recordings",
    "recording_config_directory": "config/record",
    "core_assets_directory": "downloads",
    "assets_directory": "assets",
    "dynamic_wallpapers_directory": "assets/wallpapers",
    "thumbnails_directory": "thumbnails",
    "video_shader_dir": "shaders",
    "video_filter_dir": "filters/video",
    "audio_filter_dir": "filters/audio",
    "overlay_directory": "overlays",
    "osk_overlay_directory": "overlays/keyboards",
    "cheat_database_path": "cheats",
    "content_database_path": "database/rdb",
}

# Folders KIT_DIR_KEYS names that stage does not already create through
# another step (cores/ gets PUT-CORES-HERE.txt, autoconfig/ gets the
# "mister" profile) -- each of these gets its own ABOUT-THIS-FOLDER.txt so
# that `archive` (files only, see cmd_archive) does not prune it.
KIT_DIRS = {
    "info": "core information files",
    "system": "BIOS and system files cores need",
    "saves": "save files",
    "states": "save states",
    "screenshots": "screenshots",
    "playlists": "playlists and content history",
    "cache": "temporary extracted archive content",
    "logs": "RetroArch's own log files",
    "logs/runtime": "per-content runtime play-time logs",
    "config": "menu configuration files",
    "config/remaps": "input remap files",
    "recordings": "video recordings",
    "config/record": "recording configuration presets",
    "downloads": "core updater downloads",
    "assets": "menu assets",
    "assets/wallpapers": "dynamic menu wallpapers",
    "thumbnails": "playlist thumbnails",
    "shaders": "shader presets and passes",
    "filters/video": "video filters",
    "filters/audio": "audio DSP filters",
    "overlays": "on-screen overlays",
    "overlays/keyboards": "on-screen keyboard overlays",
    "cheats": "cheat files",
    "database/rdb": "content database files",
}


# ---------------------------------------------------------------------------
# Third-party components and their license texts -- ONE table.
#
# One table, so that the notice, the copied texts and the build check cannot
# drift apart: `stage` copies every text named here from the fork, `check`
# reads NOTICE.txt back against LICENSES/, and `check-notice-objects` (run by
# the CI after "Build") compares the object folders of the build with it.
# No license text is written in this file or in NOTICE.txt: each one is
# copied from the fork, by the selector of its line, and refused when its
# marker is missing from what was selected.
# ---------------------------------------------------------------------------


class TextSource(NamedTuple):
    """One license text copied into LICENSES/.

    dest    file name inside LICENSES/ (a bare name, named by a `Text:` line
            of NOTICE.txt)
    source  path of the source file in the fork, '/'-separated
    select  `file`          the whole file, byte for byte;
            `first-comment` the first comment of the file;
            `last-comment`  the last comment of the file;
            `comment-with`  the first comment that contains `marker`.
            A comment is one /* ... */ block or one run of consecutive
            // lines. Comment texts are written with LF line ends.
    marker  a string the selected text must contain, or `stage` fails
    begin   optional, comment selectors only: start at the first line that
            contains this
    end     optional, comment selectors only: stop after the first line
            (from `begin` on) that contains this
    """

    dest: str
    source: str
    select: str
    marker: str
    begin: str = ""
    end: str = ""


class Component(NamedTuple):
    """One line of the table.

    name       how NOTICE.txt names it (the notice must contain this string)
    license    SPDX-style identifier(s), as read in the source
    platforms  the builds that compile it
    kind       `compiled` (an object folder proves it), `included` (a header
               or a source included by a known object, see INCLUDE_SITES) or
               `text` (a license text shared by several components, nothing
               to prove)
    obj_dir    for `compiled`: the object folder under the build's object
               directory (obj-unix/release), '/'-separated; else ""
    texts      the license texts copied for it
    """

    name: str
    license: str
    platforms: tuple
    kind: str
    obj_dir: str
    texts: tuple


BOTH = ("windows", "linux")

# What is listed here was measured on the build of the CI (the object
# folders `deps/*` and `libretro-common/formats/libchdr` of the Windows and
# the Linux job), not recalled. The components NOTICE.txt announces besides
# these -- the emitter, libgm, LZ4, the MinGW runtime DLLs -- are staged by
# `stage` itself, from other places than the fork's own tree.
COMPONENTS = (
    # -- compiled: an object folder under obj-unix/release proves each one --
    Component(
        "mbed TLS", "Apache-2.0", BOTH, "compiled", "deps/mbedtls",
        (
            TextSource(
                "mbedtls-notice.txt", "deps/mbedtls/aes.c", "first-comment",
                "Licensed under the Apache License",
            ),
        ),
    ),
    Component(
        "SPIRV-Cross", "Apache-2.0 OR MIT", BOTH, "compiled", "deps/SPIRV-Cross",
        (
            TextSource(
                "SPIRV-Cross-notice.txt", "deps/SPIRV-Cross/spirv_cross.hpp",
                "first-comment", "Arm Limited",
            ),
        ),
    ),
    Component(
        "glslang", "BSD-3-Clause, MIT, Apache-2.0, GPL-3.0-or-later with the Bison exception",
        BOTH, "compiled", "deps/glslang",
        (
            TextSource(
                "glslang-BSD-3-Clause.txt",
                "deps/glslang/glslang/glslang/MachineIndependent/ParseHelper.cpp",
                "first-comment", "Neither the name of 3Dlabs",
            ),
            TextSource(
                "glslang-Khronos-MIT.txt", "deps/glslang/glslang/SPIRV/GLSL.std.450.h",
                "first-comment", "Permission is hereby granted",
            ),
            TextSource(
                "glslang-Bison-parser-notice.txt",
                "deps/glslang/glslang/glslang/MachineIndependent/glslang_tab.cpp",
                "comment-with", "Bison implementation for Yacc-like parsers",
            ),
            TextSource(
                "glslang-Bison-exception.txt",
                "deps/glslang/glslang/glslang/MachineIndependent/glslang_tab.cpp",
                "comment-with", "As a special exception",
            ),
        ),
    ),
    Component(
        "Zstandard", "BSD-3-Clause", BOTH, "compiled", "deps/zstd",
        (
            TextSource(
                "zstd-BSD-3-Clause.txt", "deps/zstd/LICENSE", "file",
                "Redistribution and use in source and binary forms",
            ),
        ),
    ),
    Component(
        "libFLAC", "BSD-3-Clause", BOTH, "compiled", "deps/libFLAC",
        (
            TextSource(
                "libFLAC-BSD-3-Clause.txt", "deps/libFLAC/COPYING.Xiph", "file",
                "Xiph.Org Foundation",
            ),
        ),
    ),
    Component(
        "zlib", "Zlib", BOTH, "compiled", "deps/libz",
        (
            TextSource(
                "zlib-license.txt", "libretro-common/include/compat/zlib/zlib.h",
                "first-comment", "This software is provided 'as-is'",
            ),
        ),
    ),
    Component(
        "LZMA SDK", "public domain", BOTH, "compiled", "deps/7zip",
        (
            TextSource(
                "LZMA-SDK-public-domain.txt", "deps/7zip/LzmaDec.c", "first-comment",
                "Public domain",
            ),
        ),
    ),
    Component(
        "libchdr", "BSD-3-Clause", BOTH, "compiled", "libretro-common/formats/libchdr",
        (
            TextSource(
                "libchdr-BSD-3-Clause.txt", "libretro-common/formats/libchdr/libchdr_chd.c",
                "first-comment", "Neither the name 'MAME'",
            ),
        ),
    ),
    Component(
        "Switchres", "GPL-2.0-or-later", BOTH, "compiled", "deps/switchres",
        (
            TextSource(
                "Switchres-notice.txt", "deps/switchres/display.cpp", "first-comment",
                "GPL-2.0+",
            ),
        ),
    ),
    Component(
        "ibxm", "BSD-3-Clause", BOTH, "compiled", "deps/ibxm",
        (
            TextSource(
                "ibxm-BSD-3-Clause.txt", "deps/ibxm/licence.txt", "file", "Martin Cameron",
            ),
        ),
    ),
    Component(
        "yxml", "MIT", BOTH, "compiled", "deps/yxml",
        (
            TextSource(
                "yxml-MIT.txt", "deps/yxml/yxml.c", "comment-with", "Yoran Heling",
            ),
        ),
    ),
    # xdelta3 is compiled by the Windows build only (the Linux job of the CI
    # does not build deps/xdelta3); its text still ships in both kits.
    Component(
        "xdelta3", "Apache-2.0", ("windows",), "compiled", "deps/xdelta3",
        (
            TextSource(
                "xdelta3-notice.txt", "deps/xdelta3/xdelta3.c", "first-comment",
                "Joshua MacDonald", end="limitations under the License.",
            ),
        ),
    ),
    # -- included: header-only libraries and sources the build includes into
    # a known object (INCLUDE_SITES names the objects) --
    Component(
        "stb", "public domain", BOTH, "included", "",
        (
            TextSource(
                "stb_truetype-public-domain.txt", "deps/stb/stb_truetype.h",
                "first-comment", "This software is in the public domain",
                begin="LICENSE", end="as you see fit.",
            ),
            TextSource(
                "stb_image-public-domain.txt", "deps/stb/stb_image.h",
                "first-comment", "This software is in the public domain",
                begin="License:", end="however you want.",
            ),
            TextSource(
                "stb_rect_pack-public-domain.txt", "deps/stb/stb_rect_pack.h",
                "first-comment", "public domain",
                begin="stb_rect_pack.h", end="Sean Barrett 2014",
            ),
        ),
    ),
    Component(
        "dr_libs", "public domain or MIT-0", BOTH, "included", "",
        (
            TextSource(
                "dr_flac-public-domain-or-MIT-0.txt", "deps/dr/dr_flac.h",
                "last-comment", "ALTERNATIVE 1",
            ),
            TextSource(
                "dr_mp3-public-domain.txt", "deps/dr/dr_mp3.h", "comment-with",
                "free and unencumbered software",
            ),
            TextSource(
                "minimp3-CC0.txt", "deps/dr/dr_mp3.h", "comment-with",
                "publicdomain/zero",
            ),
        ),
    ),
    Component(
        "xxHash", "BSD-2-Clause", BOTH, "included", "",
        (
            TextSource(
                "xxHash-BSD-2-Clause.txt", "deps/xxHash/LICENSE", "file",
                "BSD 2-Clause License",
            ),
        ),
    ),
    Component(
        "Vulkan headers", "Apache-2.0", BOTH, "included", "",
        (
            TextSource(
                "Vulkan-headers-notice.txt", "gfx/include/vulkan/vulkan_core.h",
                "first-comment", "Apache License",
            ),
        ),
    ),
    Component(
        "libretro-common", "MIT", BOTH, "included", "",
        (
            TextSource(
                "libretro-common-MIT.txt", "libretro-common/file/file_path.c",
                "first-comment", "Permission is hereby granted",
            ),
        ),
    ),
    # -- text: full license texts that components above refer to. The Apache
    # text is the one in SPIRV-Cross's LICENSE (the copy under its LICENSES/
    # folder is reflowed); the GPL-2.0 text is the COPYING of Zstandard. --
    Component(
        "Apache-2.0 text", "Apache-2.0", BOTH, "text", "",
        (
            TextSource(
                "Apache-2.0.txt", "deps/SPIRV-Cross/LICENSE", "file",
                "Version 2.0, January 2004",
            ),
        ),
    ),
    Component(
        "GPL-2.0 text", "GPL-2.0", BOTH, "text", "",
        (
            TextSource(
                "GPL-2.0.txt", "deps/zstd/COPYING", "file", "Version 2, June 1991",
            ),
        ),
    ),
)


# The objects of a build (paths under obj-unix/release) that include a
# component with no object folder of its own -- a header-only library, or a
# library whose source is included by a file of the emitter. They are listed
# apart from the rows above on purpose: a site must survive the loss of its
# row, or `check-notice-objects` could not see that the row is missing. Read
# off the compile lines of the CI log (the file compiled, per job), not
# recalled.
#   audio_mixer.c     stb_vorbis, dr_flac, dr_mp3
#   stb.c             stb_truetype, stb_rect_pack
#   image_core.c      stb_image
#   libchdr_flac.c    dr_flac
#   uint32s_index.c   xxHash
#   vulkan.c          the Vulkan headers (through vulkan_common.h)
#   file_path.c       one of the libretro-common files
INCLUDE_SITES = (
    ("libretro-common/audio/audio_mixer.o", "stb"),
    ("libretro-common/audio/audio_mixer.o", "dr_libs"),
    ("gfx/drivers_font_renderer/stb.o", "stb"),
    ("cores/libretro-imageviewer/image_core.o", "stb"),
    ("libretro-common/formats/libchdr/libchdr_flac.o", "dr_libs"),
    ("input/bsv/uint32s_index.o", "xxHash"),
    ("gfx/drivers/vulkan.o", "Vulkan headers"),
    ("libretro-common/file/file_path.o", "libretro-common"),
)

# Third-party code outside deps/ that the build compiles into its own object
# folder (the one libretro-common subfolder that is a library of its own).
EXTRA_OBJECT_DIRS = ("libretro-common/formats/libchdr",)


def text_source_paths() -> list:
    """Every fork path the table copies a text from (the test kit archives
    exactly these, read from here and never from a second list)."""
    return sorted({t.source for c in COMPONENTS for t in c.texts})


def _all_texts() -> list:
    """Every (component, TextSource) pair, each destination once."""
    out: list = []
    seen: dict = {}
    for comp in COMPONENTS:
        for ts in comp.texts:
            if ts.dest in seen:
                if seen[ts.dest] != ts:
                    raise ValueError(f"two different sources for {ts.dest}")
                continue
            seen[ts.dest] = ts
            out.append((comp, ts))
    return out


class TextError(Exception):
    """A license text cannot be copied; the message names the table line."""


# A comment of a C or C++ source: one /* ... */ block, or one run of
# consecutive lines that start with //.
_COMMENT_RE = re.compile(r"/\*.*?\*/|(?:^[ \t]*//[^\n]*(?:\n|\Z))+", re.DOTALL | re.MULTILINE)


def _select_comment(data: bytes, ts: TextSource) -> bytes:
    """The comment (or the part of it) a comment selector designates, as
    bytes with LF line ends. latin-1 maps every byte to one character and
    back, so a source with non-ASCII bytes is copied without change."""
    text = data.decode("latin-1").replace("\r\n", "\n").replace("\r", "\n")
    units = [m.group(0).rstrip("\n") for m in _COMMENT_RE.finditer(text)]
    if not units:
        raise TextError("no comment found in the source")
    if ts.select == "first-comment":
        unit = units[0]
    elif ts.select == "last-comment":
        unit = units[-1]
    else:
        matching = [u for u in units if ts.marker in u]
        if not matching:
            raise TextError(f"no comment contains the marker {ts.marker!r}")
        unit = matching[0]
    lines = unit.split("\n")
    first = 0
    if ts.begin:
        hits = [i for i, ln in enumerate(lines) if ts.begin in ln]
        if not hits:
            raise TextError(f"begin {ts.begin!r} not found in the comment")
        first = hits[0]
    last = len(lines) - 1
    if ts.end:
        hits = [i for i in range(first, len(lines)) if ts.end in lines[i]]
        if not hits:
            raise TextError(f"end {ts.end!r} not found in the comment")
        last = hits[0]
    return ("\n".join(lines[first:last + 1]) + "\n").encode("latin-1")


def read_license_text(fork_dir: Path, comp: Component, ts: TextSource) -> bytes:
    """The bytes of one license text, selected from its source in the fork."""
    where = f"license text {ts.dest} ({comp.name}, from {ts.source})"
    src = fork_dir / Path(*ts.source.split("/"))
    if not src.is_file():
        raise TextError(f"{where}: source file not found: {src}")
    data = src.read_bytes()
    if ts.select == "file":
        if ts.begin or ts.end:
            raise TextError(f"{where}: begin/end need a comment selector")
        selected = data
    elif ts.select in ("first-comment", "last-comment", "comment-with"):
        try:
            selected = _select_comment(data, ts)
        except TextError as exc:
            raise TextError(f"{where}: {exc}") from None
    else:
        raise TextError(f"{where}: unknown selector {ts.select!r}")
    if not selected.strip():
        raise TextError(f"{where}: selected text is empty")
    if ts.marker.encode("ascii") not in selected:
        raise TextError(f"{where}: marker {ts.marker!r} not found in the selected text")
    return selected


def _kit_path(subfolder: str, platform: str) -> str:
    """The `:` prefix (fill_pathname_expand_special) plus the platform's
    own separator -- Windows keeps '\\', Linux '/', exactly as the two
    gabarits already do for `joypad_autoconfig_dir`."""
    sep = "\\" if platform == "windows" else "/"
    return ":" + sep + subfolder.replace("/", sep)


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
# Bench strings and leaks inside a binary (executable, DLL, object file).
# ---------------------------------------------------------------------------


def bench_string_findings(path: Path) -> list[tuple[str, str, str]]:
    """Every BENCH_STRINGS token found in `path`, as (path, token, encoding)
    with encoding `ascii` or `utf-16`. A plain substring search on the bytes
    of the whole file, so a token is found wherever it sits."""
    data = path.read_bytes()
    out: list[tuple[str, str, str]] = []
    for token in BENCH_STRINGS:
        if token.encode("ascii") in data:
            out.append((str(path), token, "ascii"))
        if token.encode("utf-16le") in data:
            out.append((str(path), token, "utf-16"))
    return out


def binary_leak_findings(path: Path) -> list[tuple[str, int]]:
    """Private addresses and personal paths inside `path`, as (rule, offset).
    Only the rule and the byte offset are returned, never the string found:
    a CI log is public and must not republish an address or a name."""
    data = path.read_bytes()
    out: list[tuple[str, int]] = []
    runs = [(m.start(), m.group(0).decode("ascii"), 1) for m in _ASCII_RUN_RE.finditer(data)]
    runs += [
        (m.start(), m.group(0).decode("utf-16le"), 2) for m in _UTF16_RUN_RE.finditer(data)
    ]
    for start, text, width in runs:
        for m in PRIVATE_IPV4_RE.finditer(text):
            out.append(("private-ipv4", start + width * m.start()))
        for m in MAC_RE.finditer(text):
            if not _is_doc_mac(m.group(0)):
                out.append(("mac-address", start + width * m.start()))
        for m in BINARY_HOME_PATH_RE.finditer(text):
            out.append(("home-path", start + width * m.start()))
    return sorted(out, key=lambda item: item[1])


def _binary_lines(path: Path) -> list[str]:
    """The `check-bench-strings` report lines for one file."""
    lines = [f"FAIL bench-string {p} {token} {enc}" for p, token, enc in bench_string_findings(path)]
    lines += [
        f"FAIL binary-leak {path} {rule} offset={offset}"
        for rule, offset in binary_leak_findings(path)
    ]
    return lines


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

    # -- custom.ini, empty: RetroArch's dir_check_defaults() (retroarch.c)
    # skips its whole mkdir loop when a file named "custom.ini" exists
    # relative to the process's own current directory -- checked BEFORE
    # crt-bridge.cfg is even read, so the ':'-prefixed folders below (already
    # scoped to the kit) never get a chance to matter. On Linux this loop
    # would otherwise create about twenty empty folders under
    # $XDG_CONFIG_HOME/retroarch or ~/.config/retroarch
    # (frontend_unix_get_env, platform_unix.c) in a release user's own home
    # directory. Both launchers `cd` into the kit before exec'ing the binary
    # (start-emitter.sh/.cmd), so a relative "custom.ini" always resolves
    # here. Windows already scopes those same pre-config defaults inside the
    # kit (the ':' prefix expands to the executable's own directory there),
    # so shipping this file on both platforms keeps one rule instead of a
    # per-platform one.
    (stage_dir / "custom.ini").write_text("", encoding="utf-8")
    staged.append("custom.ini")

    # -- cores/ marker, no core shipped --
    cores_dir = stage_dir / "cores"
    cores_dir.mkdir()
    (cores_dir / "PUT-CORES-HERE.txt").write_text(
        "Put your libretro cores here. No core is shipped with crt-bridge emitter.\n",
        encoding="utf-8",
    )
    staged.append("cores/PUT-CORES-HERE.txt")

    # -- Every other kit-relative folder KIT_DIR_KEYS names: each gets an
    # ABOUT-THIS-FOLDER.txt witness, or `archive` (files only) would prune
    # the folder silently even after `stage` created it. --
    for subfolder, purpose in sorted(KIT_DIRS.items()):
        folder_dir = stage_dir / Path(subfolder)
        folder_dir.mkdir(parents=True, exist_ok=True)
        marker = folder_dir / "ABOUT-THIS-FOLDER.txt"
        marker.write_text(
            f"RetroArch keeps {purpose} here, inside this kit, so that "
            "nothing is written anywhere else.\n",
            encoding="utf-8",
        )
        staged.append((Path(subfolder) / "ABOUT-THIS-FOLDER.txt").as_posix())

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

    # -- Third-party license texts: every line of COMPONENTS, copied from the
    # fork. Both kits get every text; NOTICE.txt says in prose which build
    # compiles which component. --
    try:
        for comp, ts in _all_texts():
            (licenses_dir / ts.dest).write_bytes(read_license_text(fork_dir, comp, ts))
            staged.append(f"LICENSES/{ts.dest}")
    except (TextError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

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


def _relative_to(member: Path, root: Path) -> Path:
    """Like `member.relative_to(root)`, tolerant of mixed path separators.

    On MSYS2's mingw-w64 CPython (the interpreter the release CI's Windows
    job builds and packages with), `Path.iterdir()`/`Path.rglob()` return
    children whose cached string form has a literal backslash at the join
    point even though every other `str(Path(...))` in the same process
    uses forward slashes (that build swaps `os.sep`/`os.altsep`, but the
    scandir-based child-path construction still hardcodes `\\`). Plain
    `relative_to()` compares those cached strings and raises `ValueError`
    -- "not in the subpath" -- even though `member` is a genuine
    descendant of `root`. `os.path.relpath` normalizes separators via
    `os.path.normpath` before comparing, so it is not fooled by the same
    mismatch, and agrees with `relative_to()` everywhere it does not
    encounter the bug (only descendants are ever passed in here, so there
    is no risk of silently producing a `..`-relative path for an
    unrelated pair).
    """
    return Path(os.path.relpath(member, root))


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
    for subfolder in sorted(KIT_DIRS):
        marker = root / Path(subfolder) / "ABOUT-THIS-FOLDER.txt"
        if marker.is_file():
            candidates.append(marker)
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


def _rule_custom_ini(root: Path) -> list[tuple[str, str]]:
    """Without this empty file at the kit root, RetroArch's
    dir_check_defaults() (retroarch.c) creates about twenty empty folders in
    a release user's own home directory before it ever reads
    crt-bridge.cfg -- measured on this branch's retroarch.c, not assumed.
    See the long comment beside where stage() writes it."""
    custom_ini = root / "custom.ini"
    if not custom_ini.is_file():
        return [("custom-ini", f"{custom_ini}: missing")]
    return []


def _rule_kit_dirs(root: Path, platform: str) -> list[tuple[str, str]]:
    """Every KIT_DIR_KEYS entry is present in crt-bridge.cfg, prefixed `:`
    + the platform separator, and its folder is physically in the staged
    directory/archive -- with a witness file, except cores/ and
    autoconfig/ (already checked by _rule_core/_rule_autoconfig_profile)."""
    findings: list[tuple[str, str]] = []
    cfg = root / "crt-bridge.cfg"
    if not cfg.is_file():
        return [("kit-dirs", f"{cfg}: missing crt-bridge.cfg")]
    parsed = parse_cfg(cfg)
    checked: set[str] = set()
    for key, subfolder in sorted(KIT_DIR_KEYS.items()):
        expected = _kit_path(subfolder, platform)
        if parsed.get(key) != expected:
            findings.append(
                ("kit-dirs", f"{cfg}: {key} = {parsed.get(key)!r}, expected {expected!r}")
            )
        if subfolder in checked:
            continue
        checked.add(subfolder)
        folder = root / Path(subfolder)
        if not folder.is_dir():
            findings.append(("kit-dirs", f"{folder}: missing"))
            continue
        if subfolder in ("cores", "autoconfig"):
            continue
        marker = folder / "ABOUT-THIS-FOLDER.txt"
        if not marker.is_file():
            findings.append(("kit-dirs", f"{marker}: missing"))
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
            str(_relative_to(p, cores_dir)) for p in cores_dir.rglob("*")
            if p.is_file() and p.name != "PUT-CORES-HERE.txt"
        )
        for rel in extra:
            findings.append(("core", f"{cores_dir / rel}: unexpected file in cores/"))
    return findings


def _rule_name(root: Path, platform: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    for member in _iter_members(root):
        rel = str(_relative_to(member, root))
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


def _rule_bench_strings(root: Path, platform: str) -> list[tuple[str, str]]:
    """Opt-in (--forbid-bench-strings): the executable and every DLL of the
    kit carry no BENCH_STRINGS token and no private address or personal path.
    Off by default because a kit staged with a bench binary for a test
    legitimately carries them. Same two functions as `check-bench-strings`,
    so both give the same verdict on the same files."""
    expected = "crt-bridge-emitter.exe" if platform == "windows" else "crt-bridge-emitter"
    binary = root / expected
    if not binary.is_file():
        return [("bench-strings", f"{binary}: missing, cannot check bench strings")]
    findings: list[tuple[str, str]] = []
    for path in [binary] + sorted(root.glob("*.dll")):
        for p, token, enc in bench_string_findings(path):
            findings.append(("bench-strings", f"{p}: {token} ({enc})"))
        for rule, offset in binary_leak_findings(path):
            findings.append(("bench-strings", f"{path}: {rule} offset={offset}"))
    return findings


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


# A notice entry names a license text with a `Text: <file name>` line.
NOTICE_TEXT_RE = re.compile(r"^[ \t]*Text:[ \t]*(\S+)[ \t]*$", re.MULTILINE)


def _notice_text_names(root: Path) -> Optional[list]:
    """The file names `LICENSES/NOTICE.txt` announces, or None when there is
    no readable NOTICE (the `licenses` rule already reports that)."""
    notice = root / "LICENSES" / "NOTICE.txt"
    if not notice.is_file():
        return None
    return NOTICE_TEXT_RE.findall(notice.read_text(encoding="utf-8", errors="replace"))


def _rule_notice_texts(root: Path) -> list[tuple[str, str]]:
    """Every `Text:` of the notice has its file, not empty, in LICENSES/."""
    names = _notice_text_names(root)
    if names is None:
        return []
    licenses_dir = root / "LICENSES"
    findings: list[tuple[str, str]] = []
    for name in names:
        if name in (".", "..") or "/" in name or "\\" in name:
            findings.append(("notice-text", f"{licenses_dir}: Text: {name} is not a bare file name"))
            continue
        p = licenses_dir / name
        if not p.is_file() or p.stat().st_size == 0:
            findings.append(("notice-text", f"{p}: announced by NOTICE.txt, missing or empty"))
    return findings


def _rule_notice_orphans(root: Path) -> list[tuple[str, str]]:
    """No file of LICENSES/ that the notice does not name. NOTICE.txt itself
    and mingw-runtime/ (one folder per MSYS2 package, copied as a whole and
    described in the notice by one sentence) are the only exceptions."""
    names = _notice_text_names(root)
    if names is None:
        return []
    licenses_dir = root / "LICENSES"
    named = set(names)
    findings: list[tuple[str, str]] = []
    for member in sorted(_iter_members(licenses_dir)):
        rel = _relative_to(member, licenses_dir).as_posix()
        if rel == "NOTICE.txt" or rel.startswith("mingw-runtime/"):
            continue
        if rel not in named:
            findings.append(("notice-orphan", f"{member}: in LICENSES/ but named by no Text: line of NOTICE.txt"))
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
    findings.extend(_rule_custom_ini(root))
    findings.extend(_rule_kit_dirs(root, args.platform))
    findings.extend(_rule_core(root, args.platform))
    findings.extend(_rule_name(root, args.platform))
    if args.require_branding:
        findings.extend(_rule_branding(root, args.platform))
    if args.forbid_bench_strings:
        findings.extend(_rule_bench_strings(root, args.platform))
    findings.extend(_rule_readme(root))
    findings.extend(_rule_licenses(root, args.platform))
    findings.extend(_rule_notice_texts(root))
    findings.extend(_rule_notice_orphans(root))
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
# check-bench-strings
# ---------------------------------------------------------------------------


def cmd_check_bench_strings(args: argparse.Namespace) -> int:
    paths = [Path(args.binary)] + [Path(d) for d in (args.dll or [])]
    lines: list[str] = []
    for path in paths:
        try:
            lines.extend(_binary_lines(path))
        except OSError as exc:
            print(f"ERROR: cannot read {path}: {exc.strerror or exc}", file=sys.stderr)
            return 2
    for line in lines:
        print(line)
    if lines:
        print(f"FAIL ({len(lines)})")
        return 1
    print("PASS")
    return 0


# ---------------------------------------------------------------------------
# check-notice-objects
# ---------------------------------------------------------------------------


def table_problems(components: Optional[Sequence] = None, sites: Optional[Sequence] = None) -> list:
    """What is inconsistent inside the table itself, as human-readable lines
    (empty when it holds together)."""
    components = COMPONENTS if components is None else components
    sites = INCLUDE_SITES if sites is None else sites
    problems: list = []
    by_name: dict = {}
    dests: dict = {}
    folders: dict = {}
    for comp in components:
        if comp.name in by_name:
            problems.append(f"component {comp.name!r} appears twice")
        by_name[comp.name] = comp
        if comp.kind not in ("compiled", "included", "text"):
            problems.append(f"{comp.name}: unknown kind {comp.kind!r}")
        if not comp.platforms or not set(comp.platforms) <= {"windows", "linux"}:
            problems.append(f"{comp.name}: platforms must be a non-empty subset of windows, linux")
        if comp.kind == "compiled":
            if not comp.obj_dir:
                problems.append(f"{comp.name}: a compiled line needs its object folder")
            elif comp.obj_dir in folders:
                problems.append(f"{comp.name}: object folder {comp.obj_dir} is also {folders[comp.obj_dir]}'s")
            else:
                folders[comp.obj_dir] = comp.name
        elif comp.obj_dir:
            problems.append(f"{comp.name}: only a compiled line has an object folder")
        if not comp.texts:
            problems.append(f"{comp.name}: no license text")
        for ts in comp.texts:
            if "/" in ts.dest or "\\" in ts.dest or not ts.dest:
                problems.append(f"{comp.name}: {ts.dest!r} is not a bare file name")
            if ts.dest in dests:
                problems.append(f"{ts.dest}: two lines ({dests[ts.dest]} and {comp.name}) for one file")
            dests[ts.dest] = comp.name
    sited: set = set()
    for obj, name in sites:
        comp = by_name.get(name)
        if comp is None:
            problems.append(f"include site {obj}: names {name}, which has no line in the table")
        elif comp.kind != "included":
            problems.append(f"include site {obj}: {name} is not an `included` line")
        sited.add(name)
    for comp in components:
        if comp.kind == "included" and comp.name not in sited:
            problems.append(f"{comp.name}: an `included` line with no include site")
    return problems


def _third_party_object_dirs(obj_dir: Path) -> dict:
    """The third-party object folders of a build that hold at least one `.o`,
    as {folder: object count}: `deps/<name>` and EXTRA_OBJECT_DIRS, whatever
    the depth of the objects below."""
    found: dict = {}
    for dirpath, _dirs, files in os.walk(obj_dir):
        count = sum(1 for f in files if f.endswith(".o"))
        if not count:
            continue
        rel = os.path.relpath(dirpath, obj_dir).replace(os.sep, "/")
        parts = rel.split("/")
        key = None
        if parts[0] == "deps" and len(parts) > 1:
            key = "deps/" + parts[1]
        else:
            for extra in EXTRA_OBJECT_DIRS:
                if rel == extra or rel.startswith(extra + "/"):
                    key = extra
        if key:
            found[key] = found.get(key, 0) + count
    return found


def notice_object_report(
    obj_dir: Path,
    platform: str,
    components: Optional[Sequence] = None,
    sites: Optional[Sequence] = None,
) -> tuple:
    """Compare the objects of a build with the table: (seen lines, findings).

    A third-party object folder with no line in the table, a line seen on a
    platform it is not for, and an include object whose component has no line
    are findings. A line of the table that the build did not produce is not:
    the table says what a build MAY carry. A build in which nothing at all is
    measured is a finding too, or a wrong --obj-dir would pass."""
    components = COMPONENTS if components is None else components
    sites = INCLUDE_SITES if sites is None else sites
    by_dir = {c.obj_dir: c for c in components if c.kind == "compiled"}
    by_name = {c.name: c for c in components}
    seen: list = []
    findings: list = []
    folders = _third_party_object_dirs(obj_dir)
    for key in sorted(folders):
        comp = by_dir.get(key)
        if comp is None:
            findings.append(
                f"FAIL notice-object {key}: compiled in this build ({folders[key]} objects), "
                "no line in the component table"
            )
        elif platform not in comp.platforms:
            findings.append(
                f"FAIL notice-object {key}: {comp.name} is a {'/'.join(comp.platforms)}-only "
                f"line of the table, seen in a {platform} build"
            )
        else:
            seen.append(f"seen folder {key} ({folders[key]} objects) -> {comp.name}")
    site_hits = 0
    for obj, name in sites:
        if not (obj_dir / obj).is_file():
            continue
        site_hits += 1
        comp = by_name.get(name)
        if comp is None:
            findings.append(f"FAIL notice-object {obj}: includes {name}, which has no line in the component table")
        elif platform not in comp.platforms:
            findings.append(
                f"FAIL notice-object {obj}: includes {name}, a {'/'.join(comp.platforms)}-only "
                f"line of the table, seen in a {platform} build"
            )
        else:
            seen.append(f"seen object {obj} -> {name}")
    if not folders and not site_hits:
        findings.append(
            "FAIL notice-object: nothing measured -- no third-party object folder and no known "
            "include object under --obj-dir (wrong directory?)"
        )
    return seen, findings


def cmd_check_notice_objects(args: argparse.Namespace) -> int:
    obj_dir = Path(args.obj_dir)
    if not obj_dir.is_dir():
        print(f"ERROR: --obj-dir not found: {obj_dir}", file=sys.stderr)
        return 2
    seen, findings = notice_object_report(obj_dir, args.platform)
    findings = [f"FAIL table {p}" for p in table_problems()] + findings
    for line in seen:
        print(line)
    for line in findings:
        print(line)
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
                arcname = f"{root_name}/{_relative_to(f, src_dir).as_posix()}"
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
                arcname = f"{root_name}/{_relative_to(f, src_dir).as_posix()}"
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
    p_check.add_argument("--forbid-bench-strings", action="store_true")

    p_bench = sub.add_parser("check-bench-strings")
    p_bench.add_argument("--binary", required=True)
    p_bench.add_argument("--dll", action="append")

    p_notice = sub.add_parser("check-notice-objects")
    p_notice.add_argument("--obj-dir", required=True)
    p_notice.add_argument("--platform", required=True, choices=["windows", "linux"])

    p_archive = sub.add_parser("archive")
    p_archive.add_argument("--platform", required=True, choices=["windows", "linux"])
    p_archive.add_argument("--dir", required=True)
    p_archive.add_argument("--out", required=True)

    args = parser.parse_args(argv)

    if args.command == "stage":
        return cmd_stage(args)
    if args.command == "check":
        return cmd_check(args)
    if args.command == "check-bench-strings":
        return cmd_check_bench_strings(args)
    if args.command == "check-notice-objects":
        return cmd_check_notice_objects(args)
    if args.command == "archive":
        return cmd_archive(args)
    return 2  # unreachable -- argparse subparsers are required


if __name__ == "__main__":
    raise SystemExit(main())
