"""Build actual patched PNGdec API tests from an explicit local dependency (no downloads)."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.library.resolve() / "src"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cpp = shutil.which("clang++") or shutil.which("g++")
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cpp or not cc:
        raise SystemExit("A native C/C++ compiler is required")
    commands = []

    def run(command):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        index = len(commands)
        (output / f"{index:02d}.log").write_bytes(result.stdout)
        commands.append({"command": command, "exit": result.returncode})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise SystemExit(f"Native PNG workspace check failed; see {output / f'{index:02d}.log'}")

    flags = ["-O2", "-DPNG_MAX_BUFFERED_PIXELS=16416", "-DZ_PREFIX", "-I" + str(source)]
    objects = []
    for name in ("PNGdec.cpp", "adler32.c", "inffast.c", "infback.c", "inflate.c", "zutil.c", "crc32.c", "inftrees.c"):
        target = output / (name + ".o")
        compiler = cpp if name.endswith(".cpp") else cc
        standard = ["-std=c++20"] if name.endswith(".cpp") else ["-std=c11"]
        run([compiler, *standard, *flags, "-c", str(source / name), "-o", str(target)])
        objects.append(str(target))
    binary = output / "workspace-api-test"
    test = Path(__file__).resolve().parents[1] / "test/png_workspace/WorkspaceApiTest.cpp"
    run([cpp, "-std=c++20", *flags, str(test), *objects, "-o", str(binary)])
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes((x * 17 + y * 29 + (x ^ y) * 3) & 255 for x in range(160)) for y in range(112))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 160, 112, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    fixture = output / "nonuniform-gray.png"
    fixture.write_bytes(png)
    run([str(binary), str(fixture)])
    c_binary = output / "workspace-c-api-test"
    c_test = test.with_name("WorkspaceCApiTest.c")
    run([cc, "-std=c11", "-D__LINUX__", *flags, str(c_test), *objects[1:], "-o", str(c_binary)])
    run([str(c_binary), str(fixture)])
    print("Actual PNG workspace C/C++ API regression: PASS")


if __name__ == "__main__":
    main()
