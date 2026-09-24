"""Утилиты: сеть, текст, токены, цвета, логи, безопасность, diff."""

from .ansi import Ansi, strip_ansi
from .diff import diff_stats, unified_diff
from .http import HttpClient, HttpError, stream_lines
from .json_utils import dumps_compact, loads_lenient, parse_json_objects
from .logging import get_logger, setup_logging
from .security import Secrets, check_command, redact
from .text import clip, is_binary, plural_ru, preview, text_table, truncate_middle
from .tokens import TokenCounter, estimate_tokens

__all__ = [
    "Ansi", "HttpClient", "HttpError", "Secrets", "TokenCounter", "check_command",
    "clip", "diff_stats", "dumps_compact", "estimate_tokens", "get_logger", "is_binary",
    "loads_lenient", "parse_json_objects", "plural_ru", "preview", "redact",
    "setup_logging", "stream_lines", "strip_ansi", "text_table", "truncate_middle",
    "unified_diff",
]
