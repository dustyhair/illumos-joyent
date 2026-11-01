#!/usr/bin/env python3
"""Build a self-contained Codex package for lx shared-network mode."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


EXPECTED_BWRAP_SHA256 = (
    "77360cb751ccedc5971391444ac86a8a33c15b04d6b4a6fe45f5d25496e62c4c"
)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vendor", type=Path, required=True)
    parser.add_argument("--wrapper", type=Path, required=True)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.output.exists():
        raise SystemExit(f"output already exists: {args.output}")

    original_codex = args.vendor / "bin" / "codex"
    original_bwrap = args.vendor / "codex-resources" / "bwrap"
    if digest(original_bwrap) != EXPECTED_BWRAP_SHA256:
        raise SystemExit("source bubblewrap digest does not match Codex 0.166.0")

    args.output.mkdir(parents=True)
    vendor_out = args.output / "vendor"
    shutil.copytree(args.vendor, vendor_out)
    shutil.copy2(args.launcher, args.output / "codex-lx")

    bundled = vendor_out / "codex-resources" / "bwrap"
    real_bwrap = bundled.with_name("bwrap.real")
    bundled.rename(real_bwrap)
    shutil.copy2(args.wrapper, bundled)
    bundled.chmod(0o755)
    real_bwrap.chmod(0o755)
    (args.output / "codex-lx").chmod(0o755)

    wrapper_digest = digest(bundled)
    codex = vendor_out / "bin" / "codex"
    image = codex.read_bytes()
    old = EXPECTED_BWRAP_SHA256.encode("ascii")
    if image.count(old) != 1:
        raise SystemExit("Codex binary does not contain one expected bwrap digest")
    codex.write_bytes(image.replace(old, wrapper_digest.encode("ascii"), 1))
    codex.chmod(0o755)

    manifest = {
        "codex_version": "0.166.0",
        "policy": (
            "shared zone network; private sandbox /dev with a zone-scoped "
            "devpts graft and ptmx alias; no CLONE_NEWNET"
        ),
        "original_codex_sha256": digest(original_codex),
        "patched_codex_sha256": digest(codex),
        "original_bwrap_sha256": digest(original_bwrap),
        "wrapper_bwrap_sha256": wrapper_digest,
        "real_bwrap_sha256": digest(real_bwrap),
    }
    (args.output / "lx-package-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
