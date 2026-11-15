"""Run the pinned Conan CLI using the selected Python, independent of PATH."""
import sys
from importlib.metadata import version
from pathlib import Path
expected = (Path(__file__).resolve().parents[1] / "conan/requirements.txt").read_text().strip().split("==")[1]
try:
    actual = version("conan")
except Exception:
    raise SystemExit("Install dependencies first: python -m pip install -r conan/requirements.txt")
if actual != expected:
    raise SystemExit(f"Conan {expected} required (found {actual}); python -m pip install -r conan/requirements.txt")
from conan.cli.cli import main
main(sys.argv[1:])
