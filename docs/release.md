# Release checklist

## Source publication

1. Review `git status --short` and the proposed diff. Include only project source, authored tests and documentation. Do not publish the working directory as a ZIP: it contains ignored private notes, game material and local tooling.
2. Run `python3 -m unittest discover -s tests`, `python3 tools/release_audit.py --history`, and `git diff --check`. The audit checks tracked and nonignored untracked files, plus all reachable Git history when requested. Fix every finding or document why it is a false positive. It is a heuristic check, not proof that no secret or copyrighted material exists.
3. Inspect `git log --all --format='%an <%ae> | %cn <%ce>' | sort -u` and any signed commits, tags, branch names and commit messages. Choose the public author name and an appropriate GitHub noreply address before committing. Local Git config, ignored files and reflogs are not included in an ordinary push; reachable commits and tags are.
4. Decide how much history to publish. To retain history, sanitize old identities and sensitive content in a separate local clone before adding a remote; this changes commit IDs and invalidates signatures. Alternatively, create a fresh repository from a reviewed source snapshot and make one initial commit. Do not push unsanitized history first and try to remove it later. Keep any private backup offline and outside the publication tree.
5. Use `git archive` of the reviewed release commit for a source snapshot, never `zip -r` of the project directory. Git archives do not include ignored dependencies; the developer build instructions explain how to supply them locally.
6. Create the GitHub repository only after review. Push only the intended branch and reviewed tags, not `--mirror` or `--all`. The owner creates the repo and approves the first publication.

## Binary packages

The release build removes debug symbols and maps source paths. Version comes from `shim/version.h`.

```sh
tools/make_dist.sh            # builds a fresh DLL and matching installer
# or: tools/make_dist.sh --dll-only
python3 tools/release_audit.py --artifact dist/Shatter-<version>.zip --artifact dist/ShatterSetup-<version>.exe
```

The ZIP should contain only `PhysXLoader.dll`, `README.txt`, `LICENSE.txt` and `shatter.ini` under its versioned folder. The installer embeds the freshly built DLL. Neither package may contain PDBs, SDK headers, vendor DLLs, logs or recordings. Review `unzip -l` and verify binary strings for machine paths; the audit also checks ASCII/UTF-16 paths and common credential patterns in package contents.

Record the source commit, toolchain versions and SHA-256 hashes of uploaded artifacts. Do not upload an existing `dist/` file merely because its filename has the right version; rebuild from the intended release source.

## Validation and release notes

- Build the release DLL and installer and run a native solver smoke test. Run oracle comparisons when the private dependencies are available; disclose tests not run and known deviations.
- Check installation, backup, upgrade and uninstall on a disposable game copy. Test missing-runtime and unsupported-runtime handling. Never use the original install as a release test fixture.
- Confirm debug output and recordings are off by default. Check settings changes and the optional slide-steering fix separately.
- Obtain in-game visual confirmation. Current evidence is Linux/Wine testing; native Windows and other store builds remain unverified until actually tested.
- State supported configurations, known limitations and how to restore the original DLL. Do not promise support across every GPU, Windows version or store build from one machine's measurements.
- The MIT license covers the project's original implementation. Third-party runtimes and game assets are not included; keep the trademark/non-affiliation notice.
- Before attaching diagnostics to a public issue, remove account names, absolute paths and unnecessary game data. Prefer the smallest reproduction using authored scenarios.
