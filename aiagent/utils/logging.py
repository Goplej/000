"""Логи агента: файл в ~/.aiagent/logs + цветная консоль при --verbose."""
from __future__ import annotations

import logging
import os
import sys
from pathlib import Path

_LOGGER_NAME = "aiagent"
_configured = False

LEVELS = {"debug": logging.DEBUG, "info": logging.INFO, "warning": logging.WARNING,
          "error": logging.ERROR}


def setup_logging(verbose: bool = False, log_file: Path | None = None, level: str = "") -> logging.Logger:
    """Настраивает логирование один раз за процесс."""
    global _configured
    logger = logging.getLogger(_LOGGER_NAME)
    if _configured:
        if verbose:
            logger.setLevel(logging.DEBUG)
        return logger

    logger.setLevel(LEVELS.get(level, logging.DEBUG if verbose else logging.INFO))
    logger.propagate = False

    formatter = logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%H:%M:%S")

    if log_file is not None:
        try:
            log_file.parent.mkdir(parents=True, exist_ok=True)
            handler = logging.FileHandler(log_file, encoding="utf-8")
            handler.setFormatter(logging.Formatter(
                "%(asctime)s %(levelname)-7s %(name)s:%(lineno)d %(message)s"
            ))
            handler.setLevel(logging.DEBUG)
            logger.addHandler(handler)
        except OSError:
            pass

    if verbose or os.environ.get("AIA_DEBUG"):
        stream = logging.StreamHandler(sys.stderr)
        stream.setFormatter(formatter)
        stream.setLevel(logging.DEBUG)
        logger.addHandler(stream)

    _configured = True
    return logger


def get_logger(name: str = "") -> logging.Logger:
    return logging.getLogger(f"{_LOGGER_NAME}.{name}" if name else _LOGGER_NAME)


def log_dir() -> Path:
    from ..paths import state_dir
    path = state_dir() / "logs"
    path.mkdir(parents=True, exist_ok=True)
    return path


__all__ = ["get_logger", "log_dir", "setup_logging"]
