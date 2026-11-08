"""Host tests for scripts/bridge_config.py (no PlatformIO/SCons required)."""

import importlib.util
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "bridge_config.py"
_spec = importlib.util.spec_from_file_location("bridge_config", SCRIPT)
bc = importlib.util.module_from_spec(_spec)
# dataclass() resolves annotations through sys.modules, so register first.
sys.modules["bridge_config"] = bc
_spec.loader.exec_module(bc)


def build(text, pioenv="esp32dev", source="bridge.ini"):
    return bc.build_from_text(text, pioenv, source=source)


def test_no_file_gives_empty_config(tmp_path, capsys):
    config = bc.load_config(tmp_path, "esp32dev", environ={})
    assert config.source == "none"
    assert config.overrides == {}
    assert config.hash == 0x811C9DC5  # FNV-1a of the empty string
    assert '#define ZB_CFG_SOURCE "none"' in config.header
    assert "#define ZB_CFG_HASH 0x811c9dc5u" in config.header
    assert "not found; using board defaults" in capsys.readouterr().out


def test_missing_zb_config_is_an_error(tmp_path):
    with pytest.raises(bc.ConfigError) as info:
        bc.load_config(tmp_path, "esp32dev", environ={"ZB_CONFIG": "missing.ini"})
    assert "missing.ini" in str(info.value)
    assert "file not found" in str(info.value)


def test_zb_config_relative_and_absolute(tmp_path):
    lobby = tmp_path / "configs" / "lobby.ini"
    lobby.parent.mkdir()
    lobby.write_text("[bridge]\nname = Lobby\n", encoding="utf-8")

    relative = bc.load_config(
        tmp_path, "esp32dev", environ={"ZB_CONFIG": "configs/lobby.ini"}
    )
    assert relative.source == "configs/lobby.ini"
    assert relative.overrides["name"] == "Lobby"

    absolute = bc.load_config(
        tmp_path, "esp32dev", environ={"ZB_CONFIG": str(lobby)}
    )
    assert absolute.source == "configs/lobby.ini"
    assert absolute.overrides["name"] == "Lobby"


def test_bridge_section_only():
    config = build(
        """
        [bridge]
        name = ZD421-Checkin
        serial = ZD421-CHECKIN-0001
        baud = 19200
        flow = cts
        log = debug
        """
    )
    assert config.overrides == {
        "name": "ZD421-Checkin",
        "serial": "ZD421-CHECKIN-0001",
        "baud": "19200",
        "flow": "cts",
        "log": "debug",
    }
    assert '#define ZB_CFG_NAME "ZD421-Checkin"' in config.header
    assert "#define ZB_CFG_BAUD 19200" in config.header
    assert "#define ZB_CFG_FLOW 2" in config.header  # console::FlowMode::Cts
    assert "#define ZB_CFG_LOG 2" in config.header  # console::LogLevel::Debug
    assert "#define ZB_CFG_HAS_NAME 1" in config.header


def test_env_section_overrides_bridge():
    text = """
    [bridge]
    name = Shared
    baud = 9600
    [esp32c3]
    tx = 6
    baud = 19200
    """
    dev = build(text, "esp32dev")
    c3 = build(text, "esp32c3")
    assert dev.overrides["baud"] == "9600"
    assert "tx" not in dev.overrides
    assert c3.overrides["baud"] == "19200"
    assert c3.overrides["tx"] == "6"

    header = c3.header
    assert "#define ZB_CFG_TX 6" in header
    assert "#define ZB_CFG_BAUD 19200" in header


def test_bom_and_crlf(tmp_path):
    path = tmp_path / "bridge.ini"
    path.write_bytes(
        b"\xef\xbb\xbf[bridge]\r\nname = BomTest\r\nserial = BOM-1\r\n"
    )
    config = bc.load_config(tmp_path, "esp32dev", environ={})
    assert config.source == "bridge.ini"
    assert config.overrides == {"name": "BomTest", "serial": "BOM-1"}


def test_comments_and_inline_comments():
    config = build(
        """
        ; full-line comment
        # another comment
        [bridge]      ; inline after section
        name = ZD#1   ; inline comment after whitespace
        serial = A;B  ; semicolon comment
        baud = 9600
        """
    )
    assert config.overrides["name"] == "ZD#1"
    assert config.overrides["serial"] == "A;B"
    assert config.overrides["baud"] == "9600"


@pytest.mark.parametrize(
    ("pioenv", "section", "line", "key"),
    [
        ("esp32dev", "bridge", "name = " + "a" * 30, "name"),
        ("esp32dev", "bridge", "serial = " + "b" * 33, "serial"),
        ("esp32dev", "bridge", "name = ", "name"),
        ("esp32dev", "bridge", "name = caf\u00e9", "name"),
        ("esp32dev", "bridge", "serial = caf\u00e9", "serial"),
        ("esp32dev", "bridge", "baud = 12345", "baud"),
        ("esp32dev", "bridge", "baud = abc", "baud"),
        ("esp32dev", "bridge", "baud = 9_600", "baud"),
        ("esp32dev", "bridge", "flow = magic", "flow"),
        ("esp32dev", "bridge", "log = loud", "log"),
        ("esp32dev", "bridge", "ledlow = 2", "ledlow"),
        ("esp32dev", "bridge", "tx = 34", "tx"),  # input-only on WROOM
        ("esp32dev", "bridge", "rx = 6", "rx"),  # SPI flash on WROOM
        ("esp32dev", "bridge", "tx = -1", "tx"),
        ("esp32dev", "bridge", "cts = 99", "cts"),
        ("esp32dev", "bridge", "rx = nope", "rx"),
        ("esp32c3", "bridge", "tx = 20", "tx"),  # ROM UART0
        ("esp32c3", "bridge", "tx = 12", "tx"),  # SPI flash
        ("esp32c3", "esp32c3", "tx = 21", "tx"),
    ],
)
def test_invalid_values_rejected(pioenv, section, line, key):
    text = "[{}]\n{}\n".format(section, line)
    with pytest.raises(bc.ConfigError) as info:
        build(text, pioenv)
    message = str(info.value)
    assert "bridge.ini" in message
    assert key in message


def test_valid_boundary_values():
    config = build(
        """
        [bridge]
        name = {name}
        serial = {serial}
        baud = 115200
        flow = none
        log = error
        ledlow = 1
        cts = -1
        led = -1
        tx = 0
        rx = 33
        """.format(name="a" * 29, serial="b" * 32)
    )
    assert len(config.overrides["name"]) == 29
    assert config.overrides["cts"] == "-1"
    assert config.overrides["led"] == "-1"
    assert config.overrides["tx"] == "0"
    assert "#define ZB_CFG_LED -1" in config.header


def test_unknown_key_rejected():
    with pytest.raises(bc.ConfigError) as info:
        build("[bridge]\nbogus = 1\n")
    assert "bogus" in str(info.value)
    assert "unknown key" in str(info.value)


def test_unknown_section_rejected():
    with pytest.raises(bc.ConfigError) as info:
        build("[native]\ntx = 1\n")
    assert "native" in str(info.value)
    assert "unknown section" in str(info.value)


def test_string_escaping():
    config = build('[bridge]\nname = A"B\\C\nserial = S\n')
    assert '#define ZB_CFG_NAME "A\\"B\\\\C"' in config.header
    # The escape sequence must round-trip back to the original text.
    assert 'A\\"B\\\\C' in config.header


def test_hash_stable_across_key_order_and_sensitive_to_values():
    first = build("[bridge]\nname = One\nserial = S1\nbaud = 9600\n")
    reordered = build("[bridge]\nbaud = 9600\nname = One\nserial = S1\n")
    changed = build("[bridge]\nname = Two\nserial = S1\nbaud = 9600\n")

    assert first.hash == reordered.hash
    assert first.hash != changed.hash
    assert "0x{:08x}u".format(first.hash) in first.header


def test_empty_file_equals_no_file_hash():
    config = build("")
    assert config.overrides == {}
    assert config.hash == bc.empty_build_config().hash
