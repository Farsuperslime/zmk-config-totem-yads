#!/usr/bin/env python3
"""Generate the GitLab CI child pipeline for a ZMK user config repository.

Port of zmkfirmware/zmk/.github/workflows/build-user-config.yml (v0.3.0), the
reusable workflow the GitHub Actions file in this repository called.  GitHub
resolved the build matrix from build.yaml at run time; GitLab cannot call
GitHub reusable workflows, so the matrix is resolved here instead and emitted
as one child pipeline job per entry plus a job that merges the firmware.

Usage:
    python3 .gitlab/generate-pipeline.py --matrix build.yaml --config config \
        --out generated-ci.yml
"""

from __future__ import annotations

import argparse
import os
import re
import sys

import yaml

BUILD_IMAGE = "zmkfirmware/zmk-build-arm:stable"
CACHE_KEY = "zmk-west-modules-v2"
CACHE_PATHS = (
    ".west-workspace/modules/",
    ".west-workspace/tools/",
    ".west-workspace/zephyr/",
    ".west-workspace/bootloader/",
    ".west-workspace/zmk/",
)
EXPIRE_IN = "1 month"
BUILD_DIR = "$CI_PROJECT_DIR/build"
ARTIFACT_DIR = "$CI_PROJECT_DIR/build/artifacts"
ARTIFACT_PATH = "build/artifacts"

_NAME_RE = re.compile(r"[^a-z0-9]+")


def _represent_string(dumper, data: str):
    """Emit multi-line script bodies as literal blocks, everything else plain."""
    return dumper.represent_scalar(
        "tag:yaml.org,2002:str", data, style="|" if "\n" in data else None
    )


yaml.add_representer(str, _represent_string, Dumper=yaml.Dumper)


class ConfigError(Exception):
    """A build.yaml or command line value that cannot be turned into a job."""


def describe(entry: dict) -> str:
    board = entry.get("board", "<no board>")
    shield = entry.get("shield")
    return f"{shield} / {board}" if shield else str(board)


def one_line(value, field: str, entry: dict) -> str:
    text = str(value)
    if "\n" in text or "\r" in text:
        raise ConfigError(f"{field} of {describe(entry)} must be a single line")
    return text


def quoted(value: str) -> str:
    """Double-quote a build.yaml value, suppressing shell expansion."""
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$")
    return f'"{escaped}"'


def load_entries(matrix_path: str) -> list[dict]:
    try:
        with open(matrix_path, encoding="utf-8") as handle:
            document = yaml.safe_load(handle)
    except OSError as error:
        raise ConfigError(f"cannot read {matrix_path}: {error}") from error
    except yaml.YAMLError as error:
        raise ConfigError(f"{matrix_path} is not valid YAML: {error}") from error

    entries = document.get("include") if isinstance(document, dict) else document
    if not isinstance(entries, list) or not entries:
        raise ConfigError(
            f"{matrix_path}: expected a list of build entries, either at the top "
            f"level or under 'include:'"
        )
    for entry in entries:
        if not isinstance(entry, dict):
            raise ConfigError(f"{matrix_path}: build entries must be mappings, got {entry!r}")
        if not entry.get("board"):
            raise ConfigError(f"{matrix_path}: every build entry needs a 'board'")
    return entries


def job_name(artifact_name: str, taken: set[str]) -> str:
    slug = _NAME_RE.sub("-", artifact_name.lower()).strip("-") or "build"
    candidate, index = f"build-{slug}", 2
    while candidate in taken:
        candidate, index = f"build-{slug}-{index}", index + 1
    taken.add(candidate)
    return candidate


def build_script(config_dir: str, shield: bool, snippet: bool, module_mode: bool, cmake_args: bool) -> str:
    """West invocation, assembled like the GitHub workflow's variable soup."""
    lines = [f'set -- build -s zmk/app -d "{BUILD_DIR}" -b "$BOARD"']
    if snippet:
        lines.append('if [ -n "$SNIPPET" ]; then set -- "$@" -S "$SNIPPET"; fi')
    lines.append(f'set -- "$@" -- -DZMK_CONFIG={config_dir}')
    if shield:
        lines.append('if [ -n "$SHIELD" ]; then set -- "$@" -DSHIELD="$SHIELD"; fi')
    if module_mode:
        lines.append('set -- "$@" -DZMK_EXTRA_MODULES="$CI_PROJECT_DIR"')
    if cmake_args:
        # Unquoted on purpose: word splitting is what makes extra flags work,
        # exactly as GitHub substituted them into its shell script.
        lines.append('set -- "$@" $CMAKE_ARGS')
    lines.append('west "$@"')
    return "\n".join(lines)


def rename_script(fallback_binary: str) -> str:
    """Copy the built firmware to its artifact name (mirrors 'Rename artifacts')."""
    return "\n".join(
        [
            f'if [ -f "{BUILD_DIR}/zephyr/zmk.uf2" ]; then',
            f'  cp "{BUILD_DIR}/zephyr/zmk.uf2" "{ARTIFACT_DIR}/$ARTIFACT_NAME.uf2"',
            f'elif [ -f "{BUILD_DIR}/zephyr/zmk.{fallback_binary}" ]; then',
            f'  cp "{BUILD_DIR}/zephyr/zmk.{fallback_binary}" '
            f'"{ARTIFACT_DIR}/$ARTIFACT_NAME.{fallback_binary}"',
            "fi",
        ]
    )


def build_job(entry: dict, args, module_mode: bool) -> dict:
    board = one_line(entry["board"], "board", entry)
    shield = entry.get("shield") and one_line(entry["shield"], "shield", entry)
    snippet = entry.get("snippet") and one_line(entry["snippet"], "snippet", entry)
    cmake_args = entry.get("cmake-args") and one_line(entry["cmake-args"], "cmake-args", entry)
    artifact_name = one_line(
        entry.get("artifact-name") or f"{shield + '-' if shield else ''}{board}-zmk",
        "artifact-name",
        entry,
    )

    if module_mode:
        # Modules build from a west workspace inside CI_PROJECT_DIR (so GitLab's
        # project-relative cache paths can actually reach it), separate from the
        # module checkout itself, which is passed in via ZMK_EXTRA_MODULES.
        west_root = "$CI_PROJECT_DIR/.west-workspace"
        config_dir = f'"{west_root}/{args.config}"'
        before_script = [
            'git config --global url."https://${PROSPECTOR_GIT_USER:-oauth2}:${PROSPECTOR_ACCESS_TOKEN}@gitlab.com/lcthrock/".insteadOf "https://gitlab.com/lcthrock/"',
            f'rm -rf "{west_root}/{args.config}"',
            f'mkdir -p "{west_root}/{args.config}"',
            f'cp -R "$CI_PROJECT_DIR/{args.config}"/. "{west_root}/{args.config}"/',
            f"west init -l {config_dir}",
            f'cd "{west_root}"',
            "west update --fetch-opt=--filter=tree:0",
            "west zephyr-export",
        ]
    else:
        config_dir = f'"$CI_PROJECT_DIR/{args.config}"'
        before_script = [
            'git config --global url."https://${PROSPECTOR_GIT_USER:-oauth2}:${PROSPECTOR_ACCESS_TOKEN}@gitlab.com/lcthrock/".insteadOf "https://gitlab.com/lcthrock/"',
            f"west init -l {config_dir}",
            "west update --fetch-opt=--filter=tree:0",
            "west zephyr-export",
        ]

    after_script = "\n".join(
        [
            "set +e",
            f'if [ -f "{BUILD_DIR}/zephyr/.config" ]; then',
            f"  grep -v -e '^#' -e '^$' \"{BUILD_DIR}/zephyr/.config\" | sort",
            "else",
            '  echo "No Kconfig output"',
            "fi",
            f'if [ -f "{BUILD_DIR}/zephyr/zephyr.dts" ]; then',
            f'  cat "{BUILD_DIR}/zephyr/zephyr.dts"',
            f'elif [ -f "{BUILD_DIR}/zephyr/zephyr.dts.pre" ]; then',
            f'  cat -s "{BUILD_DIR}/zephyr/zephyr.dts.pre"',
            "else",
            '  echo "No Devicetree output"',
            "fi",
        ]
    )

    return artifact_name, {
        "extends": ".build",
        "variables": {
            "BOARD": board,
            "SHIELD": shield or "",
            "SNIPPET": snippet or "",
            "CMAKE_ARGS": cmake_args or "",
            "ARTIFACT_NAME": artifact_name,
        },
        "before_script": before_script,
        "script": [
            f'mkdir -p "{ARTIFACT_DIR}"',
            build_script(config_dir, bool(shield), bool(snippet), module_mode, bool(cmake_args)),
            rename_script(args.fallback_binary),
        ],
        "after_script": [after_script],
        "artifacts": {
            "name": f"artifact-{artifact_name}",
            "paths": [f"{ARTIFACT_PATH}/"],
            "expire_in": EXPIRE_IN,
        },
    }


def generate(args) -> str:
    entries = load_entries(args.matrix)
    project_dir = os.path.dirname(os.path.abspath(args.matrix))
    module_mode = os.path.isfile(os.path.join(project_dir, "zephyr", "module.yml"))
    if not os.path.isdir(os.path.join(project_dir, args.config)):
        raise ConfigError(f"config directory {args.config!r} does not exist next to {args.matrix}")

    taken: set[str] = set()
    jobs: dict[str, dict] = {}
    notes: list[str] = []
    for entry in entries:
        artifact, job = build_job(entry, args, module_mode)
        name = job_name(artifact, taken)
        jobs[name] = job
        notes.append(f"#   {name}: {describe(entry)} -> artifact-{artifact}")
    if "merge" in jobs:
        raise ConfigError("a build entry produced a job named 'merge'")

    pipeline = {
        "stages": ["build", "merge"],
        ".build": {
            "stage": "build",
            "image": BUILD_IMAGE,
            "cache": {"key": CACHE_KEY, "paths": list(CACHE_PATHS)},
        },
        **jobs,
        "merge": {
            "stage": "merge",
            "image": BUILD_IMAGE,
            "needs": list(jobs),
            "script": [f'ls -l "{ARTIFACT_DIR}/"'],
            "artifacts": {
                "name": args.archive_name,
                "paths": [f"{ARTIFACT_PATH}/"],
                "expire_in": EXPIRE_IN,
            },
        },
    }

    workspace = "$CI_PROJECT_DIR/.west-workspace (module layout)" if module_mode else "$CI_PROJECT_DIR"
    header = [
        "# Generated by .gitlab/generate-pipeline.py from build.yaml.",
        "# Do not edit: regenerated on every pipeline run from build.yaml.",
        "#",
        f"# config directory: {args.config}",
        f"# west workspace:   {workspace}",
        f"# jobs:             {len(jobs)} build + 1 merge",
        "#",
    ]
    header += notes
    header.append("")
    return "\n".join(header) + yaml.dump(
        pipeline,
        Dumper=yaml.Dumper,
        sort_keys=False,
        default_flow_style=False,
        width=1000,
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--matrix", default="build.yaml", help="build matrix file")
    parser.add_argument("--config", default="config", help="config directory")
    parser.add_argument("--fallback-binary", default="bin", help="binary used when no .uf2 exists")
    parser.add_argument("--archive-name", default="firmware", help="merged artifact name")
    parser.add_argument("--out", default="-", help="output file, or - for stdout")
    args = parser.parse_args(argv)

    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.config):
        print(f"error: unsupported config directory name {args.config!r}", file=sys.stderr)
        return 2

    try:
        pipeline = generate(args)
    except ConfigError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.out == "-":
        sys.stdout.write(pipeline)
    else:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(pipeline)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
