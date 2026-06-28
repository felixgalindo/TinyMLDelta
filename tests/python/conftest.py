"""Shared pytest fixtures / path setup for TinyMLDelta tests.

Makes the PatchGen CLI importable and locatable from the tests regardless of
where pytest is invoked from.
"""
import os
import subprocess
import sys
import tempfile

import pytest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CLI_DIR = os.path.join(REPO_ROOT, "cli")
PATCHGEN = os.path.join(CLI_DIR, "tinymldelta_patchgen.py")

# Make `import tinymldelta_patchgen` work in unit tests.
sys.path.insert(0, CLI_DIR)


@pytest.fixture
def make_patch(tmp_path):
    """Return a function (base_bytes, target_bytes) -> patch_bytes via PatchGen."""
    def _make(base: bytes, target: bytes, algo: str = "crc32") -> bytes:
        bp = tmp_path / "base.bin"
        tp = tmp_path / "target.bin"
        pp = tmp_path / "patch.tmd"
        bp.write_bytes(base)
        tp.write_bytes(target)
        res = subprocess.run(
            [sys.executable, PATCHGEN, str(bp), str(tp), str(pp), "--algo", algo],
            capture_output=True, text=True,
        )
        assert res.returncode == 0, f"patchgen failed: {res.stderr}"
        return pp.read_bytes()
    return _make
