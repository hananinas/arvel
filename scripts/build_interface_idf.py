#!/usr/bin/env python3
#   ESP32 SvelteKit adapted for ESP-IDF
#
#   A simple, secure and extensible framework for IoT projects for ESP32 platforms
#   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
#   https://github.com/theelims/ESP32-sveltekit
#
#   Copyright (C) 2018 - 2023 rjwats
#   Copyright (C) 2023 - 2024 theelims
#   Copyright (C) 2023 Maxtrium B.V. [ code available under dual license ]
#   Copyright (C) 2024 runeharlyk
#   Copyright (C) 2025 hmbacher
#
#   All Rights Reserved. This software may be modified and distributed under
#   the terms of the LGPL v3 license. See the LICENSE file for details.

from pathlib import Path
from shutil import copytree, rmtree, copyfileobj
from os.path import exists, getmtime
import os
import sys
import gzip
import mimetypes
import glob
import subprocess
from datetime import datetime

# Get project directory from command line argument
if len(sys.argv) < 2:
    print("Error: Project directory not specified")
    sys.exit(1)

project_dir = sys.argv[1]
interface_dir = os.path.join(project_dir, "interface")
output_file = os.path.join(project_dir, "main", "WWWData.h")
source_www_dir = os.path.join(interface_dir, "src")
build_dir = os.path.join(interface_dir, "build")

# For ESP-IDF, we always embed WWW (PROGMEM style)
EMBED_WWW = True


def find_latest_timestamp_for_app():
    files = glob.glob(f"{source_www_dir}/**/*", recursive=True)
    if not files:
        return 0
    return max((getmtime(f) for f in files if os.path.isfile(f)))


def should_regenerate_output_file():
    if not exists(output_file):
        return True

    last_source_change = find_latest_timestamp_for_app()
    last_build = getmtime(output_file)

    print(
        f"Newest file: {datetime.fromtimestamp(last_source_change)}, output file: {datetime.fromtimestamp(last_build)}"
    )

    return last_build < last_source_change


def get_package_manager():
    if exists(os.path.join(interface_dir, "pnpm-lock.yaml")):
        return "pnpm"
    if exists(os.path.join(interface_dir, "yarn.lock")):
        return "yarn"
    else:
        return "npm"


def build_webapp():
    package_manager = get_package_manager()
    print(f"Building interface with {package_manager}")
    original_dir = os.getcwd()

    try:
        os.chdir(interface_dir)

        # Install dependencies
        print(f"Running {package_manager} install...")
        result = subprocess.run([package_manager, "install"], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Error installing dependencies: {result.stderr}")
            sys.exit(1)

        # Build the interface
        print(f"Running {package_manager} run build...")
        result = subprocess.run([package_manager, "run", "build"], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Error building interface: {result.stderr}")
            sys.exit(1)

        print("Interface built successfully")

    finally:
        os.chdir(original_dir)


def build_progmem():
    mimetypes.init()

    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)

    with open(output_file, "w") as progmem:
        progmem.write("#ifndef WWW_DATA_H\n")
        progmem.write("#define WWW_DATA_H\n\n")
        progmem.write("#include <stdint.h>\n")
        progmem.write("#include <stddef.h>\n\n")

        assetMap = {}

        for idx, path in enumerate(Path(build_dir).rglob("*.*")):
            asset_path = path.relative_to(build_dir).as_posix()
            asset_mime = (
                mimetypes.guess_type(asset_path)[0] or "application/octet-stream"
            )
            print(f"Converting {asset_path}")

            asset_var = f"WWW_DATA_{idx}"
            progmem.write(f"// {asset_path}\n")
            progmem.write(f"static const uint8_t {asset_var}[] = {{\n\t")
            file_data = gzip.compress(path.read_bytes())

            for i, byte in enumerate(file_data):
                if i and not (i % 16):
                    progmem.write("\n\t")
                progmem.write(f"0x{byte:02X},")

            progmem.write("\n};\n\n")
            assetMap[asset_path] = {
                "name": asset_var,
                "mime": asset_mime,
                "size": len(file_data),
            }

        # Generate structure for ESP-IDF
        progmem.write("typedef struct {\n")
        progmem.write("\tconst char* uri;\n")
        progmem.write("\tconst char* content_type;\n")
        progmem.write("\tconst uint8_t* content;\n")
        progmem.write("\tsize_t len;\n")
        progmem.write("} www_data_t;\n\n")

        progmem.write(f"#define WWW_DATA_COUNT {len(assetMap)}\n\n")
        progmem.write("static const www_data_t www_data[] = {\n")

        for asset_path, asset in assetMap.items():
            progmem.write(
                f'\t{{.uri = "/{asset_path}", .content_type = "{asset["mime"]}", '
                f'.content = {asset["name"]}, .len = {asset["size"]}}},\n'
            )

        progmem.write("};\n\n")
        progmem.write("#endif // WWW_DATA_H\n")


def embed_webapp():
    print("Converting interface to embedded data (PROGMEM style)")
    build_progmem()


print("="*60)
print("Building web interface for ESP-IDF")
print("="*60)

if should_regenerate_output_file():
    build_webapp()
    embed_webapp()
    print("Web interface build completed successfully")
else:
    print("Web interface is up to date, skipping build")
