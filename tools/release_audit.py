#!/usr/bin/env python3
"""Heuristic pre-publication checks for source, reachable history and release artifacts.

Reports categories and locations, never matched credential values. Does not modify Git.
"""

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile


PATTERNS = {
    "personal home path": re.compile(
        rb"/(?:home|Users)/[^/\s\"<>]+|[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s\"<>]+"
    ),
    "email address": re.compile(
        rb"(?<![\w@])[A-Za-z0-9_.+-]+@[A-Za-z0-9_.-]+\.[A-Za-z]{2,}(?![\w@])"
    ),
    "possible credential": re.compile(
        rb"gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|"
        rb"AKIA[0-9A-Z]{16}|sk-(?:proj-)?[A-Za-z0-9_-]{32,}|"
        rb"-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----"
    ),
}
PRIVATE_ROOTS = {"vendor-dlls", "third_party", ".toolchain", "build", "dist", "scratchpad", ".cache"}
BINARY_SUFFIXES = {".dll", ".exe", ".pdb", ".obj", ".lib", ".res", ".zip", ".bundle", ".bin"}
LOCAL_SUFFIXES = {".pxcr", ".pxsd", ".log", ".trace", ".pem", ".key", ".upk", ".me1", ".umap", ".u"}
PACKAGE_FILES = {"PhysXLoader.dll", "README.txt", "LICENSE.txt", "shatter.ini"}
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def path_problem(name):
    parts = PurePosixPath(name).parts
    if not parts:
        return None
    if parts[0] in PRIVATE_ROOTS or parts[0].startswith(".wine-"):
        return "private/generated location"
    if name in {"PERSONAL.md", "docs/HANDOFF.md", ".local.env", "compile_commands.json"}:
        return "private/generated file"
    if any(p == ".env" or (p.startswith(".env.") and p != ".env.example") for p in parts):
        return "environment file"
    if parts[0] == "captures" and name != "captures/.gitkeep":
        return "capture data"
    if len(parts) > 1 and parts[0] == "re" and parts[1] in {"out", "logs", "ghidra"}:
        return "vendor reverse-engineering output"
    if PurePosixPath(name).suffix.lower() in BINARY_SUFFIXES | LOCAL_SUFFIXES:
        return "binary, game data or diagnostic file"
    return None


def content_problems(data, binary=False):
    # PE resources and embedded paths may be UTF-16LE and start at either byte alignment.
    views = [data]
    if binary:
        views.extend(data[start:].decode("utf-16le", errors="ignore").encode("utf-8") for start in (0, 1))
    findings = set()
    for label, pattern in PATTERNS.items():
        for view in views:
            matches = pattern.finditer(view)
            if label == "email address":
                matches = (m for m in matches if not m.group().lower().endswith(b"@users.noreply.github.com"))
            if next(matches, None) is not None:
                findings.add(label)
    if not binary and b"\0" in data:
        findings.add("non-text source file")
    if binary and (b"RSDS" in data or b"NB10" in data):
        findings.add("possible CodeView/PDB debug record")
    return findings


def check_source(root, findings):
    names = set(git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard").split(b"\0"))
    for raw in sorted(names - {b""}):
        name = os.fsdecode(raw)
        problem = path_problem(name)
        if problem:
            findings.add((name, problem))
        path = root / name
        if path.is_symlink():
            data = os.fsencode(os.readlink(path))
            if os.path.isabs(os.readlink(path)):
                findings.add((name, "absolute symlink target"))
        elif path.is_file():
            data = path.read_bytes()
        else:
            findings.add((name, "missing file or submodule; review separately"))
            continue
        for problem in content_problems(data):
            findings.add((name, problem))
    return len(names - {b""})


def check_history(root, findings):
    # Check every historical filename, not just cat-file's representative path for a shared blob.
    for raw in git(root, "log", "--all", "--format=", "--name-only", "-z").split(b"\0"):
        name = os.fsdecode(raw).lstrip("\n")
        problem = path_problem(name)
        if problem:
            findings.add(("history: " + name, problem))
    objects = git(root, "rev-list", "--objects", "--all").splitlines()
    count = 0
    with subprocess.Popen(
        ["git", "-C", str(root), "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    ) as proc:
        for obj in objects:
            oid, _, raw_name = obj.partition(b" ")
            proc.stdin.write(oid + b"\n")
            proc.stdin.flush()
            header = proc.stdout.readline().split()
            if len(header) != 3:
                raise RuntimeError("could not read a reachable Git object")
            data = proc.stdout.read(int(header[2]))
            proc.stdout.read(1)
            kind = header[1]
            if kind not in {b"blob", b"commit", b"tag"}:
                continue
            count += 1
            location = "history: " + (os.fsdecode(raw_name) if kind == b"blob" else "commit/tag metadata")
            for problem in content_problems(data):
                findings.add((location, problem))
        proc.stdin.close()
        if proc.wait() != 0:
            raise RuntimeError("git cat-file failed")
    return count


def check_artifact(path, findings):
    def check_bytes(location, data, binary):
        if binary and not data.startswith(b"MZ"):
            findings.add((location, "expected a Windows PE binary"))
        for problem in content_problems(data, binary=binary):
            findings.add((location, problem))

    if path.stat().st_size > MAX_ARTIFACT_BYTES:
        findings.add((str(path), "artifact too large for this audit"))
        return
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            files = [i for i in archive.infolist() if not i.is_dir()]
            names = {PurePosixPath(i.filename).name for i in files}
            folders = {str(PurePosixPath(i.filename).parent) for i in files}
            if len(files) != len(PACKAGE_FILES) or names != PACKAGE_FILES or len(folders) != 1:
                findings.add((str(path), "unexpected ZIP contents"))
            if sum(i.file_size for i in files) > MAX_ARTIFACT_BYTES:
                findings.add((str(path), "uncompressed artifact too large for this audit"))
                return
            for info in archive.infolist():
                name = PurePosixPath(info.filename)
                if name.is_absolute() or ".." in name.parts or "\\" in info.filename:
                    findings.add((str(path), "unsafe ZIP member path"))
                if info.is_dir():
                    continue
                check_bytes(str(path) + ": " + info.filename, archive.read(info), name.suffix.lower() == ".dll")
    elif path.suffix.lower() in {".dll", ".exe"}:
        check_bytes(str(path), path.read_bytes(), True)
    else:
        findings.add((str(path), "unsupported artifact type (use ZIP, DLL or EXE)"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", action="store_true", help="also check all reachable commits, tags and blobs")
    parser.add_argument("--artifact", type=Path, action="append", default=[], help="check a release ZIP, DLL or EXE")
    args = parser.parse_args()
    findings = set()
    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip())
        source_count = check_source(root, findings)
        print(f"Checked {source_count} tracked/nonignored source files.")
        if args.history:
            print(f"Checked {check_history(root, findings)} reachable history objects.")
        for path in args.artifact:
            check_artifact(path, findings)
    except (OSError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile) as exc:
        print(f"Audit could not complete: {exc}", file=sys.stderr)
        return 2
    for location, problem in sorted(findings):
        print(f"FAIL: {location}: {problem}")
    if findings:
        print("Review findings before publishing. Matched values are deliberately omitted.")
        return 1
    print("PASS: no findings in the checked scope. Manual review is still required.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
