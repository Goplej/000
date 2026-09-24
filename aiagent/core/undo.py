"""Чекпоинты и откат: правки агента всегда можно вернуть назад.

Перед изменением файла делаем снимок в ~/.aiagent/projects/<проект>/checkpoints.
Каждый шаг агента = один чекпоинт; /undo в CLI и POST /api/undo откатывают шаги.
"""
from __future__ import annotations

import json
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..config import AgentConfig
from ..paths import project_paths
from ..utils.logging import get_logger

log = get_logger("undo")
MAX_STEPS = 80


@dataclass
class FileChange:
    path: str
    backup: str = ""          # путь к копии (пусто = файла не было)
    existed: bool = False
    action: str = "write"

    def to_dict(self) -> dict:
        return {"path": self.path, "backup": self.backup, "existed": self.existed,
                "action": self.action}


@dataclass
class Checkpoint:
    index: int
    title: str
    created: float = field(default_factory=time.time)
    changes: list[FileChange] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {"index": self.index, "title": self.title, "created": self.created,
                "changes": [c.to_dict() for c in self.changes]}


class CheckpointManager:
    """История изменений файлов с возможностью отката."""

    def __init__(self, config: AgentConfig, workspace: Path | None = None) -> None:
        self.config = config
        self.paths = project_paths(workspace or config.workspace)
        self.dir = self.paths.checkpoints_dir
        self.checkpoints: list[Checkpoint] = []
        self._current: Checkpoint | None = None
        self._snapshots: dict[str, FileChange] = {}

    # ------------------------------------------------------------------- запись
    def begin(self, title: str) -> None:
        self._current = Checkpoint(index=len(self.checkpoints) + 1, title=title)
        self._snapshots = {}

    def snapshot(self, path: str | Path) -> None:
        """Запомнить состояние файла до изменения."""
        file_path = Path(path)
        key = str(file_path)
        if key in self._snapshots:
            return
        change = FileChange(path=key, existed=file_path.exists(), action="update")
        if file_path.is_file():
            name = f"{int(time.time() * 1000)}-{abs(hash(key)) % 10 ** 8}-{file_path.name}"
            backup = self.dir / name
            try:
                shutil.copy2(file_path, backup)
                change.backup = str(backup)
            except OSError as e:
                log.warning("Не удалось сохранить копию %s: %s", file_path, e)
        self._snapshots[key] = change

    def commit(self, title: str = "") -> Checkpoint | None:
        """Завершить шаг: изменения попадают в историю."""
        if self._current is None:
            return None
        checkpoint = self._current
        if title:
            checkpoint.title = title
        checkpoint.changes = list(self._snapshots.values())
        self._current = None
        self._snapshots = {}
        if not checkpoint.changes:
            return None
        self.checkpoints.append(checkpoint)
        self._trim()
        self.save_index()
        return checkpoint

    def discard(self) -> None:
        self._current = None
        self._snapshots = {}

    def _trim(self) -> None:
        while len(self.checkpoints) > MAX_STEPS:
            old = self.checkpoints.pop(0)
            for change in old.changes:
                _remove(change.backup)

    # --------------------------------------------------------------------- откат
    def undo(self, count: int = 1) -> str:
        if not self.checkpoints:
            return "Откатывать нечего: агент ещё ничего не менял."
        lines: list[str] = []
        for _ in range(max(1, int(count))):
            if not self.checkpoints:
                break
            checkpoint = self.checkpoints.pop()
            restored, deleted = 0, 0
            for change in reversed(checkpoint.changes):
                path = Path(change.path)
                try:
                    if change.existed and change.backup and Path(change.backup).is_file():
                        path.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(change.backup, path)
                        restored += 1
                    elif path.exists():
                        path.unlink()
                        deleted += 1
                except OSError as e:
                    lines.append(f"  ! не удалось откатить {self._rel(path)}: {e}")
                finally:
                    _remove(change.backup)
            lines.append(f"шаг {checkpoint.index} «{checkpoint.title}»: "
                         f"восстановлено {restored}, удалено {deleted}")
        self.save_index()
        return "\n".join(lines)

    def history(self, limit: int = 20) -> str:
        if not self.checkpoints:
            return "История изменений пуста."
        out = []
        for checkpoint in self.checkpoints[-limit:]:
            files = ", ".join(self._rel(Path(c.path)) for c in checkpoint.changes) or "—"
            moment = time.strftime("%H:%M:%S", time.localtime(checkpoint.created))
            out.append(f"{checkpoint.index:>3}. [{moment}] {checkpoint.title} → {files}")
        return "\n".join(out)

    # ------------------------------------------------------------------ служебное
    def _rel(self, path: Path) -> str:
        try:
            return str(path.relative_to(Path(self.config.workspace)))
        except ValueError:
            return str(path)

    def save_index(self) -> None:
        try:
            (self.dir / "index.json").write_text(
                json.dumps([c.to_dict() for c in self.checkpoints], ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except OSError:
            pass

    def clear(self) -> None:
        for checkpoint in self.checkpoints:
            for change in checkpoint.changes:
                _remove(change.backup)
        self.checkpoints.clear()
        self.save_index()

    def summary(self) -> str:
        changed = {self._rel(Path(c.path)) for cp in self.checkpoints for c in cp.changes}
        return (f"шагов с изменениями: {len(self.checkpoints)}, уникальных файлов: {len(changed)}")


def _remove(path: str) -> None:
    if not path:
        return
    try:
        Path(path).unlink(missing_ok=True)
    except OSError:
        pass


__all__ = ["Checkpoint", "CheckpointManager", "FileChange"]
