"""Build-time bridge configuration for zebra-bridge-esp32.

This file is used in two ways:

* As a PlatformIO ``pre`` extra script for the ESP32 environments.  In that
  role it reads ``bridge.ini`` (or the file named by ``ZB_CONFIG``), validates
  the values, and generates ``$BUILD_DIR/generated/bridge_config_gen.h``.
* Imported as a plain Python module by ``tests/test_bridge_config.py``.  All
  parsing / validation / rendering lives in pure functions that do not need
  SCons, so the tests can exercise them on the host.

The INI format follows the Pi bridge's ``zebra-bridge.conf``: sections,
``key = value`` pairs, ``;`` and ``#`` comments, UTF-8 (with optional BOM) and
CRLF line endings.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Mapping, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants mirrored from the firmware (console.cpp / console.h).
# ---------------------------------------------------------------------------

DEFAULT_NAME = "ZD421-Emulation"

SECTIONS = ("bridge", "esp32dev", "esp32c3")

KEYS = (
    "name",
    "serial",
    "baud",
    "flow",
    "tx",
    "rx",
    "cts",
    "led",
    "ledlow",
    "log",
)

KEYS_BY_ENV = ("esp32dev", "esp32c3")

BAUD_RATES = {
    1200,
    2400,
    4800,
    9600,
    14400,
    19200,
    28800,
    38400,
    57600,
    115200,
}

FLOW_VALUES = {"none": 0, "xonxoff": 1, "cts": 2}
LOG_VALUES = {"error": 0, "info": 1, "debug": 2}

# GPIO validity tables copied from console.cpp.
ESP32_GPIOS = {
    0,
    1,
    2,
    3,
    4,
    5,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    21,
    22,
    23,
    25,
    26,
    27,
    32,
    33,
}
ESP32C3_GPIOS = set(range(0, 11)) | {18, 19}

MAX_NAME_LENGTH = 29
MAX_SERIAL_LENGTH = 32
MAX_GPIO = 48

FNV1A_OFFSET_BASIS = 0x811C9DC5
FNV1A_PRIME = 0x01000193


class ConfigError(Exception):
    """A configuration error that should stop the build."""

    def __init__(
        self,
        reason: str,
        path: Optional[str] = None,
        section: Optional[str] = None,
        key: Optional[str] = None,
    ) -> None:
        self.path = path
        self.section = section
        self.key = key
        self.reason = reason
        super().__init__(str(self))

    def __str__(self) -> str:
        parts = []
        if self.path:
            parts.append(str(self.path))
        if self.section:
            parts.append("[{}]".format(self.section))
        if self.key:
            parts.append(str(self.key))
        prefix = ": ".join(parts)
        return "{}: {}".format(prefix, self.reason) if prefix else self.reason


@dataclass
class BuildConfig:
    source: str
    overrides: Dict[str, str]
    hash: int
    header: str


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


def _strip_inline_comment(value: str) -> str:
    """Remove a ``;`` / ``#`` comment that follows whitespace.

    ``name = ZD#1`` keeps the ``#`` because it is not preceded by whitespace;
    ``baud = 9600  ; printer default`` drops the comment.  This matches the
    usual INI convention used by Python's configparser inline comments.
    """

    for index, character in enumerate(value):
        if character in ";#" and (index == 0 or value[index - 1].isspace()):
            return value[:index]
    return value


def parse_config(text: str, filename: str = "<config>") -> Dict[str, Dict[str, str]]:
    """Parse INI text into ``{section: {key: value}}``.

    Raises :class:`ConfigError` for malformed lines, unknown sections, and
    duplicate sections/keys.
    """

    sections: Dict[str, Dict[str, str]] = {}
    current: Optional[str] = None

    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith((";", "#")):
            continue

        if line.startswith("["):
            close = line.find("]")
            if close < 0:
                raise ConfigError(
                    "line {}: unterminated section header".format(line_number),
                    path=filename,
                )
            section = line[1:close].strip()
            rest = line[close + 1 :].strip()
            if rest and not rest.startswith((";", "#")):
                raise ConfigError(
                    "line {}: unexpected text after section header".format(
                        line_number
                    ),
                    path=filename,
                )
            if section not in SECTIONS:
                raise ConfigError(
                    "unknown section [{}] (allowed: {})".format(
                        section, ", ".join("[{}]".format(s) for s in SECTIONS)
                    ),
                    path=filename,
                    section=section,
                )
            if section in sections:
                raise ConfigError(
                    "line {}: duplicate section".format(line_number),
                    path=filename,
                    section=section,
                )
            sections[section] = {}
            current = section
            continue

        if current is None:
            raise ConfigError(
                "line {}: key/value outside of a section".format(line_number),
                path=filename,
            )

        if "=" not in line:
            raise ConfigError(
                "line {}: expected 'key = value'".format(line_number),
                path=filename,
                section=current,
            )

        raw_key, raw_value = line.split("=", 1)
        key = raw_key.strip().lower()
        if not key:
            raise ConfigError(
                "line {}: empty key".format(line_number),
                path=filename,
                section=current,
            )
        if key not in KEYS:
            raise ConfigError(
                "unknown key (allowed: {})".format(", ".join(KEYS)),
                path=filename,
                section=current,
                key=key,
            )
        value = _strip_inline_comment(raw_value).strip()
        if key in sections[current]:
            raise ConfigError(
                "line {}: duplicate key".format(line_number),
                path=filename,
                section=current,
                key=key,
            )
        sections[current][key] = value

    return sections


def resolve_overrides(
    sections: Mapping[str, Mapping[str, str]],
    pioenv: str,
    filename: str = "<config>",
) -> Dict[str, str]:
    """Merge ``[bridge]`` with the section named after ``pioenv``."""

    if pioenv not in KEYS_BY_ENV:
        raise ConfigError(
            "unsupported PlatformIO environment '{}'".format(pioenv),
            path=filename,
        )

    resolved: Dict[str, str] = {}
    if "bridge" in sections:
        resolved.update(sections["bridge"])
    if pioenv in sections:
        resolved.update(sections[pioenv])
    return resolved


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def _is_printable_ascii(value: str) -> bool:
    return all(0x20 <= ord(character) <= 0x7E for character in value)


def _validate_string(
    key: str,
    value: str,
    maximum: int,
    path: str,
    section: str,
) -> str:
    if not value or len(value) > maximum:
        raise ConfigError(
            "must be 1..{} characters".format(maximum),
            path=path,
            section=section,
            key=key,
        )
    if not _is_printable_ascii(value):
        raise ConfigError(
            "must be printable ASCII",
            path=path,
            section=section,
            key=key,
        )
    return value


_DECIMAL_RE = re.compile(r"[+-]?[0-9]+")


def _parse_decimal(key: str, value: str, path: str, section: str) -> int:
    # Mirror strtol(): ASCII digits with an optional sign.  Python's int()
    # alone would also accept "1_0" and non-ASCII digits.
    if not _DECIMAL_RE.fullmatch(value):
        raise ConfigError(
            "invalid integer '{}'".format(value),
            path=path,
            section=section,
            key=key,
        )
    return int(value, 10)


def _gpio_allowed(pioenv: str, gpio: int) -> bool:
    if pioenv == "esp32c3":
        return gpio in ESP32C3_GPIOS
    return gpio in ESP32_GPIOS


def validate_overrides(
    raw_overrides: Mapping[str, str],
    pioenv: str,
    filename: str = "<config>",
    section: str = "bridge",
) -> Dict[str, str]:
    """Validate raw values and return a normalised ``key -> str`` mapping.

    ``section`` only affects error messages.  Callers validating the env
    section pass its name so a bad value is reported against the right block.
    """

    normalised: Dict[str, str] = {}

    for key, value in raw_overrides.items():
        if key in ("name", "serial"):
            maximum = MAX_NAME_LENGTH if key == "name" else MAX_SERIAL_LENGTH
            normalised[key] = _validate_string(
                key, value, maximum, filename, section
            )
            continue

        if key == "baud":
            baud = _parse_decimal(key, value, filename, section)
            if baud not in BAUD_RATES:
                raise ConfigError(
                    "must be one of {}".format(
                        " ".join(str(rate) for rate in sorted(BAUD_RATES))
                    ),
                    path=filename,
                    section=section,
                    key=key,
                )
            normalised[key] = str(baud)
            continue

        if key == "flow":
            lowered = value.lower()
            if lowered not in FLOW_VALUES:
                raise ConfigError(
                    "must be one of none, xonxoff, cts",
                    path=filename,
                    section=section,
                    key=key,
                )
            normalised[key] = lowered
            continue

        if key == "log":
            lowered = value.lower()
            if lowered not in LOG_VALUES:
                raise ConfigError(
                    "must be one of error, info, debug",
                    path=filename,
                    section=section,
                    key=key,
                )
            normalised[key] = lowered
            continue

        if key == "ledlow":
            if value not in ("0", "1"):
                raise ConfigError(
                    "must be 0 or 1",
                    path=filename,
                    section=section,
                    key=key,
                )
            normalised[key] = value
            continue

        if key in ("tx", "rx", "cts", "led"):
            gpio = _parse_decimal(key, value, filename, section)
            if gpio < -1 or gpio > MAX_GPIO:
                raise ConfigError(
                    "invalid GPIO number '{}'".format(value),
                    path=filename,
                    section=section,
                    key=key,
                )
            allow_disabled = key in ("cts", "led")
            if gpio == -1:
                if not allow_disabled:
                    raise ConfigError(
                        "this pin cannot be disabled (-1)",
                        path=filename,
                        section=section,
                        key=key,
                    )
            elif not _gpio_allowed(pioenv, gpio):
                raise ConfigError(
                    "GPIO is not valid for this target",
                    path=filename,
                    section=section,
                    key=key,
                )
            normalised[key] = str(gpio)
            continue

        # parse_config() rejects unknown keys, so this is unreachable.
        raise ConfigError(
            "unknown key",
            path=filename,
            section=section,
            key=key,
        )

    return normalised


# ---------------------------------------------------------------------------
# Hashing and header rendering
# ---------------------------------------------------------------------------


def compute_hash(overrides: Mapping[str, str]) -> int:
    """FNV-1a 32-bit over sorted ``key=value\\n`` lines."""

    digest = FNV1A_OFFSET_BASIS
    for key in sorted(overrides):
        line = "{}={}\n".format(key, overrides[key]).encode("utf-8")
        for byte in line:
            digest ^= byte
            digest = (digest * FNV1A_PRIME) & 0xFFFFFFFF
    return digest


def c_string_literal(value: str) -> str:
    """Return ``value`` as a C string literal with quotes/backslashes escaped."""

    pieces = ['"']
    for character in value:
        code = ord(character)
        if character == "\\":
            pieces.append("\\\\")
        elif character == '"':
            pieces.append('\\"')
        elif character == "\n":
            pieces.append("\\n")
        elif character == "\r":
            pieces.append("\\r")
        elif character == "\t":
            pieces.append("\\t")
        elif 0x20 <= code <= 0x7E:
            pieces.append(character)
        else:
            # Three-digit octal escapes cannot swallow the next character.
            pieces.append("\\{:03o}".format(code))
    pieces.append('"')
    return "".join(pieces)


def _header_value(key: str, value: str) -> str:
    if key in ("name", "serial"):
        return c_string_literal(value)
    if key == "flow":
        return str(FLOW_VALUES[value])
    if key == "log":
        return str(LOG_VALUES[value])
    return str(int(value))


def render_header(source: str, hash_value: int, overrides: Mapping[str, str]) -> str:
    """Render ``bridge_config_gen.h``."""

    lines = [
        "// Generated by scripts/bridge_config.py. Do not edit.",
        "#pragma once",
        "",
        "#define ZB_CFG_SOURCE {}".format(c_string_literal(source)),
        "#define ZB_CFG_HASH 0x{:08x}u".format(hash_value & 0xFFFFFFFF),
        "",
    ]

    for key in KEYS:
        if key not in overrides:
            continue
        macro = key.upper()
        lines.append("#define ZB_CFG_HAS_{} 1".format(macro))
        lines.append(
            "#define ZB_CFG_{} {}".format(macro, _header_value(key, overrides[key]))
        )

    return "\n".join(lines) + "\n"


def build_from_text(
    text: str,
    pioenv: str,
    source: str = "bridge.ini",
    filename: Optional[str] = None,
) -> BuildConfig:
    """Parse, validate and render a configuration from INI text."""

    path = filename or source
    sections = parse_config(text, path)
    if pioenv not in KEYS_BY_ENV:
        raise ConfigError(
            "unsupported PlatformIO environment '{}'".format(pioenv),
            path=path,
        )
    overrides: Dict[str, str] = {}
    for section in ("bridge", pioenv):
        if section in sections:
            overrides.update(
                validate_overrides(sections[section], pioenv, path, section)
            )
    hash_value = compute_hash(overrides)
    return BuildConfig(
        source=source,
        overrides=overrides,
        hash=hash_value,
        header=render_header(source, hash_value, overrides),
    )


def empty_build_config() -> BuildConfig:
    """The "no bridge.ini" result: no overrides, constant empty hash."""

    overrides: Dict[str, str] = {}
    hash_value = compute_hash(overrides)
    return BuildConfig(
        source="none",
        overrides=overrides,
        hash=hash_value,
        header=render_header("none", hash_value, overrides),
    )


# ---------------------------------------------------------------------------
# File loading and the PlatformIO entry point
# ---------------------------------------------------------------------------


def _display_path(path: Path, project_root: Path) -> str:
    try:
        relative = path.relative_to(project_root)
    except ValueError:
        relative = Path(os.path.relpath(str(path), str(project_root)))
    return relative.as_posix()


def load_config(
    project_root: Path,
    pioenv: str,
    environ: Optional[Mapping[str, str]] = None,
) -> BuildConfig:
    """Load the configured file, or the empty config when it is absent.

    A missing default ``bridge.ini`` is not an error.  A missing file named by
    ``ZB_CONFIG`` raises :class:`ConfigError`.
    """

    if environ is None:
        environ = os.environ

    configured = (environ.get("ZB_CONFIG") or "").strip()
    if configured:
        path = Path(configured)
        if not path.is_absolute():
            path = project_root / path
        explicit = True
    else:
        path = project_root / "bridge.ini"
        explicit = False

    if not path.exists():
        if explicit:
            raise ConfigError(
                "file not found (ZB_CONFIG={})".format(configured), path=str(path)
            )
        print(
            "bridge config: {} not found; using board defaults".format(
                _display_path(path, project_root)
            )
        )
        return empty_build_config()

    source = _display_path(path, project_root)
    try:
        text = path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError:
        raise ConfigError("file is not valid UTF-8", path=source) from None
    except OSError as error:
        raise ConfigError(
            "cannot read file: {}".format(error), path=source
        ) from None

    return build_from_text(text, pioenv, source=source, filename=source)


def _write_if_changed(path: Path, content: str) -> None:
    if path.exists():
        try:
            if path.read_text(encoding="utf-8") == content:
                return
        except OSError:
            pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _entry_point(env) -> None:
    project_root = Path(env.subst("$PROJECT_DIR"))
    pioenv = env["PIOENV"]

    config = load_config(project_root, pioenv)

    build_dir = Path(env.subst("$BUILD_DIR"))
    generated_dir = build_dir / "generated"
    header_path = generated_dir / "bridge_config_gen.h"
    _write_if_changed(header_path, config.header)

    env.Append(CPPPATH=[str(generated_dir)])

    name = config.overrides.get("name", DEFAULT_NAME)
    print(
        "bridge config: source={} hash=0x{:08x} name={}".format(
            config.source, config.hash, name
        )
    )


# When PlatformIO executes this file as an extra script, SCons injects
# ``Import`` into the module globals.  Imported as a library (pytest) this
# block is skipped.
if "Import" in globals():  # pragma: no cover - exercised only by PlatformIO
    Import("env")  # noqa: F821

    try:
        _entry_point(env)  # noqa: F821
    except ConfigError as error:
        print("error: {}".format(error), file=sys.stderr)
        env.Exit(1)  # noqa: F821
