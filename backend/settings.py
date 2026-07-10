"""Runtime LLM credentials/config, persisted to appdata (the DATA_DIR volume).

Lets the operator enter OpenAI/Anthropic keys (and optional model overrides) in the
web UI after the container is installed, instead of pre-seeding a .env on the host.

Resolution: the appdata settings file WINS; any value provided via the container
environment (.env / -e) is the fallback/bootstrap. Values are applied into
os.environ so the anthropic/openai SDKs (which read the env) pick them up unchanged.

Secrets are stored in plaintext in appdata (same trust level as a .env on the host).
The app has no auth, so keep the server on your LAN.
"""
from __future__ import annotations

import json
import os
import stat
from typing import Any

from store import DATA_DIR

try:  # defaults are only for display ("what model will actually be used")
    from compare import DEFAULT_CLAUDE_MODEL, DEFAULT_GPT_MODEL, DEFAULT_WHISPER_MODEL
except Exception:  # pragma: no cover - keep settings importable even if compare changes
    DEFAULT_WHISPER_MODEL = "whisper-1"
    DEFAULT_GPT_MODEL = "gpt-5.5"
    DEFAULT_CLAUDE_MODEL = "claude-sonnet-5"

SETTINGS_PATH = DATA_DIR / "settings.json"

SECRET_KEYS = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY")
MODEL_KEYS = ("WHISPER_MODEL", "CLAUDE_MODEL", "GPT_MODEL")
ANKI_KEYS = ("ANKI_CONNECT_URL", "ANKI_DECK")
PROVIDER_KEYS = ("LLM_PROVIDER",)
FLAG_KEYS = ("AUTO_ANKI",)  # boolean-ish flags stored as "1"/"0"
MANAGED_KEYS = SECRET_KEYS + MODEL_KEYS + ANKI_KEYS + PROVIDER_KEYS + FLAG_KEYS

_MODEL_DEFAULTS = {
    "WHISPER_MODEL": DEFAULT_WHISPER_MODEL,
    "CLAUDE_MODEL": DEFAULT_CLAUDE_MODEL,
    "GPT_MODEL": DEFAULT_GPT_MODEL,
}

# Snapshot the container/host environment at import so the appdata file can override
# it and clearing a field can fall back to (or fully drop below) it.
_ENV_BASELINE = {name: os.environ.get(name) for name in MANAGED_KEYS}


def auto_anki_enabled() -> bool:
    """Whether each upload auto-saves to Anki and syncs. Default ON when unset."""
    return os.getenv("AUTO_ANKI", "1").strip().lower() not in ("0", "false", "off", "no")


def preferred_llm_provider() -> str:
    """Primary enrichment provider. The other provider is used as fallback."""
    value = os.getenv("LLM_PROVIDER", "anthropic").strip().lower()
    return value if value in ("anthropic", "openai") else "anthropic"


def _read_file() -> dict[str, Any]:
    if not SETTINGS_PATH.exists():
        return {}
    try:
        data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, OSError):
        return {}


def _write_file(data: dict[str, Any]) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(SETTINGS_PATH)
    try:  # best-effort tighten perms (no-op on Windows)
        SETTINGS_PATH.chmod(stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        pass


def apply_to_env() -> None:
    """Reconcile os.environ with persisted settings: appdata file wins, then the
    original container/host env as fallback, else the key is left unset."""
    data = _read_file()
    for name in MANAGED_KEYS:
        value = data.get(name) or _ENV_BASELINE.get(name)
        if value:
            os.environ[name] = str(value)
        else:
            os.environ.pop(name, None)


def update_settings(updates: dict[str, str | None]) -> dict[str, Any]:
    """Merge provided fields into the appdata file and re-apply to env.

    A value of "" (or None) clears that field. Omitted fields are left untouched.
    """
    data = _read_file()
    for name in MANAGED_KEYS:
        if name not in updates:
            continue
        value = updates[name]
        if value is None or str(value).strip() == "":
            data.pop(name, None)
        else:
            data[name] = str(value).strip()
    _write_file(data)
    apply_to_env()
    return status()


def _mask(value: str) -> str:
    if len(value) <= 8:
        return "•" * len(value)
    return f"{value[:3]}…{value[-4:]}"


def status() -> dict[str, Any]:
    """Masked, env-effective view for the UI. Never returns raw secrets."""
    secrets = {}
    for name in SECRET_KEYS:
        value = os.getenv(name, "")
        short = name.split("_")[0].lower()  # openai / anthropic
        secrets[short] = {"configured": bool(value), "hint": _mask(value) if value else ""}

    models = {}
    for name in MODEL_KEYS:
        short = name.split("_")[0].lower()  # whisper / claude / gpt
        value = os.getenv(name)
        models[short] = {
            "value": value or _MODEL_DEFAULTS[name],
            "is_default": not bool(value),
        }

    return {
        "secrets": secrets,
        "models": models,
        "llm_provider": preferred_llm_provider(),
        "anki": {
            "connect_url": os.getenv("ANKI_CONNECT_URL", ""),
            "deck": os.getenv("ANKI_DECK", "Default"),
        },
    }
