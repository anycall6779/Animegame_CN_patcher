#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path


OLD_APP = "com.combosdk.openapi.ComboApplication"
NEW_APP = "com.combosdk.openapi.ComboAppProxy"
LIB_PATH = "lib/arm64-v8a/libanimegame_native_localify.so"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def patch_binary_manifest(data: bytes) -> bytes:
    old = OLD_APP.encode("utf-16le")
    new = NEW_APP.encode("utf-16le")
    if len(new) > len(old):
        raise RuntimeError("new application class is longer than original; in-place AXML patch is not possible")

    manifest = bytearray(data)
    off = manifest.find(old)
    if off < 0:
        if NEW_APP.encode("utf-16le") in manifest:
            return bytes(manifest)
        raise RuntimeError(f"Could not find {OLD_APP} in binary AndroidManifest.xml")

    if off < 2:
        raise RuntimeError("invalid UTF-16 string offset in AndroidManifest.xml")

    old_len = struct.unpack_from("<H", manifest, off - 2)[0]
    if old_len != len(OLD_APP):
        raise RuntimeError(f"unexpected application string length: {old_len}, expected {len(OLD_APP)}")

    struct.pack_into("<H", manifest, off - 2, len(NEW_APP))
    manifest[off : off + len(new)] = new
    # Keep the AXML string pool/chunk size unchanged. The new UTF-16 string is shorter,
    # so terminate it and clear the unused bytes inside the old string slot.
    manifest[off + len(new) : off + len(old) + 2] = b"\x00" * (len(old) + 2 - len(new))
    return bytes(manifest)


def should_skip_original_entry(name: str) -> bool:
    if name == LIB_PATH:
        return True
    if name == "AndroidManifest.xml":
        return True
    # Old signatures are invalid after any modification. The successful sample was rebuilt
    # without the original META-INF service/metadata files, so omit META-INF entirely.
    if name.startswith("META-INF/"):
        return True
    return False


def write_entry(zout: zipfile.ZipFile, name: str, data: bytes, compress_type: int) -> None:
    info = zipfile.ZipInfo(name)
    info.compress_type = compress_type
    info.external_attr = 0o644 << 16
    zout.writestr(info, data)


def next_classes_dex_path(zin: zipfile.ZipFile) -> str:
    max_dex = 0
    for name in zin.namelist():
        if not name.startswith("classes") or not name.endswith(".dex"):
            continue
        suffix = name[len("classes") : -len(".dex")]
        if suffix == "":
            index = 1
        else:
            try:
                index = int(suffix)
            except ValueError:
                continue
        max_dex = max(max_dex, index)
    next_dex = max_dex + 1
    return "classes.dex" if next_dex <= 1 else f"classes{next_dex}.dex"


def patch_apk(original_apk: Path, output_apk: Path, classes5: Path, native_lib: Path) -> str:
    if not original_apk.exists():
        raise FileNotFoundError(original_apk)
    if not classes5.exists():
        raise FileNotFoundError(classes5)
    if not native_lib.exists():
        raise FileNotFoundError(native_lib)

    output_apk.parent.mkdir(parents=True, exist_ok=True)
    temp = output_apk.with_suffix(output_apk.suffix + ".tmp")
    if temp.exists():
        temp.unlink()

    with zipfile.ZipFile(original_apk, "r") as zin, zipfile.ZipFile(temp, "w") as zout:
        loader_dex_path = next_classes_dex_path(zin)
        manifest = patch_binary_manifest(zin.read("AndroidManifest.xml"))
        original_manifest_info = zin.getinfo("AndroidManifest.xml")
        write_entry(zout, "AndroidManifest.xml", manifest, original_manifest_info.compress_type)

        for info in zin.infolist():
            if should_skip_original_entry(info.filename):
                continue
            if info.filename == loader_dex_path:
                raise RuntimeError(f"loader dex path already exists: {loader_dex_path}")
            data = zin.read(info.filename)
            new_info = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            new_info.compress_type = info.compress_type
            new_info.external_attr = info.external_attr
            new_info.comment = info.comment
            zout.writestr(new_info, data)

        write_entry(zout, loader_dex_path, classes5.read_bytes(), zipfile.ZIP_DEFLATED)
        write_entry(zout, LIB_PATH, native_lib.read_bytes(), zipfile.ZIP_STORED)

    if output_apk.exists():
        output_apk.unlink()
    temp.rename(output_apk)
    return loader_dex_path


def run(cmd: list[str]) -> None:
    print("+ " + " ".join(str(x) for x in cmd))
    code = subprocess.call([str(x) for x in cmd])
    if code != 0:
        raise RuntimeError(f"command failed with exit code {code}")


def find_zipalign(explicit: Path | None = None) -> Path | None:
    if explicit is not None:
        return explicit if explicit.exists() else None

    candidates: list[Path] = []
    for env_name in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        value = os.environ.get(env_name)
        if value:
            candidates.extend(Path(value).glob("build-tools/*/zipalign.exe"))
            candidates.extend(Path(value).glob("build-tools/*/zipalign"))

    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        sdk = Path(local_app_data) / "Android" / "Sdk"
        candidates.extend(sdk.glob("build-tools/*/zipalign.exe"))
        candidates.extend(sdk.glob("build-tools/*/zipalign"))

    candidates = [p for p in candidates if p.exists()]
    if not candidates:
        return shutil.which("zipalign") and Path(shutil.which("zipalign"))  # type: ignore[arg-type]
    return sorted(candidates, key=lambda p: p.parent.name, reverse=True)[0]


def build_payload_with_powershell(root: Path, gjs: Path | None, ndk_root: Path | None) -> None:
    script = root / "build_payload.ps1"
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File", str(script)]
    if gjs is not None:
        cmd.extend(["-Gjs", str(gjs)])
    if ndk_root is not None:
        cmd.extend(["-NdkRoot", str(ndk_root)])
    run(cmd)


def ensure_debug_keystore(path: Path) -> None:
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            "keytool",
            "-genkeypair",
            "-v",
            "-keystore",
            path,
            "-storepass",
            "android",
            "-keypass",
            "android",
            "-alias",
            "androiddebugkey",
            "-keyalg",
            "RSA",
            "-keysize",
            "2048",
            "-validity",
            "10000",
            "-dname",
            "CN=Android Debug,O=Android,C=US",
        ]
    )


def align_apk(input_apk: Path, aligned_apk: Path, zipalign: Path) -> None:
    if not zipalign.exists():
        raise FileNotFoundError(f"zipalign not found: {zipalign}")
    if aligned_apk.exists():
        aligned_apk.unlink()
    run([zipalign, "-p", "-f", "4", input_apk, aligned_apk])


def sign_apk(unsigned_apk: Path, signed_apk: Path, apksigner: Path, keystore: Path, zipalign: Path | None) -> None:
    if not apksigner.exists():
        raise FileNotFoundError(f"apksigner jar not found: {apksigner}")
    ensure_debug_keystore(keystore)
    signed_apk.parent.mkdir(parents=True, exist_ok=True)
    sign_input = unsigned_apk
    if zipalign is not None:
        aligned_apk = signed_apk.with_name(signed_apk.stem + "_aligned.apk")
        align_apk(unsigned_apk, aligned_apk, zipalign)
        sign_input = aligned_apk
    run(
        [
            "java",
            "-jar",
            apksigner,
            "sign",
            "--ks",
            keystore,
            "--ks-key-alias",
            "androiddebugkey",
            "--ks-pass",
            "pass:android",
            "--key-pass",
            "pass:android",
            "--out",
            signed_apk,
            sign_input,
        ]
    )


def verify_output(apk: Path, loader_dex_path: str) -> None:
    with zipfile.ZipFile(apk, "r") as z:
        names = set(z.namelist())
        missing = [p for p in ("AndroidManifest.xml", loader_dex_path, LIB_PATH) if p not in names]
        if missing:
            raise RuntimeError(f"patched APK missing entries: {missing}")
        manifest = z.read("AndroidManifest.xml")
        if NEW_APP.encode("utf-16le") not in manifest:
            raise RuntimeError("patched AndroidManifest.xml does not contain ComboAppProxy")
        if OLD_APP.encode("utf-16le") in manifest:
            raise RuntimeError("patched AndroidManifest.xml still contains ComboApplication")


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Patch China/Bilibili APK directly, without apktool.")
    parser.add_argument("apk", type=Path, help="Original.StripResource or bhxy2 Bilibili APK")
    parser.add_argument("--out", type=Path, default=here / "out" / "animegame_localify_unsigned.apk")
    parser.add_argument("--classes5", type=Path, default=here / "payload" / "classes5.dex")
    parser.add_argument("--native-lib", type=Path, default=here / "payload" / "libanimegame_native_localify.so")
    parser.add_argument("--build-payload", action="store_true", help="Generate classes5.dex and native .so from source before patching.")
    parser.add_argument("--gjs", type=Path, help="g.js containing translation_default for --build-payload.")
    parser.add_argument("--ndk-root", type=Path, help="Android NDK root for --build-payload.")
    parser.add_argument("--sign", action="store_true", help="Also create a signed APK with apksigner.jar")
    parser.add_argument("--signed-out", type=Path, default=here / "out" / "animegame_localify_signed.apk")
    parser.add_argument("--apksigner", type=Path, default=Path("auto-singer-main") / "apksigner.jar")
    parser.add_argument("--zipalign", type=Path, help="zipalign executable. Auto-detected from Android SDK when omitted.")
    parser.add_argument("--keystore", type=Path, default=here / "debug.keystore")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        here = Path(__file__).resolve().parent
        if args.build_payload:
            build_payload_with_powershell(here, args.gjs.resolve() if args.gjs else None, args.ndk_root.resolve() if args.ndk_root else None)
        loader_dex_path = patch_apk(args.apk.resolve(), args.out.resolve(), args.classes5.resolve(), args.native_lib.resolve())
        verify_output(args.out.resolve(), loader_dex_path)
        print(f"Loader dex: {loader_dex_path}")
        print(f"Unsigned APK: {args.out.resolve()}")
        print(f"sha256: {sha256(args.out.resolve())}")
        if args.sign:
            apksigner = args.apksigner
            if not apksigner.is_absolute():
                apksigner = (Path.cwd() / apksigner).resolve()
            zipalign = find_zipalign(args.zipalign.resolve() if args.zipalign else None)
            if zipalign is None:
                raise RuntimeError("zipalign was not found. Install Android SDK build-tools or pass --zipalign.")
            sign_apk(args.out.resolve(), args.signed_out.resolve(), apksigner, args.keystore.resolve(), zipalign)
            print(f"Signed APK: {args.signed_out.resolve()}")
            print(f"sha256: {sha256(args.signed_out.resolve())}")
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
