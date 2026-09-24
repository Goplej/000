"""Определение типа проекта и его типовых команд (запуск, тесты, линтер)."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

try:                       # tomllib есть в Python 3.11+; для 3.10 пробуем tomli
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ModuleNotFoundError:
        tomllib = None  # type: ignore[assignment]

MARKERS: dict[str, tuple[str, ...]] = {
    "python": ("pyproject.toml", "setup.py", "requirements.txt", "Pipfile"),
    "node": ("package.json",),
    "rust": ("Cargo.toml",),
    "go": ("go.mod",),
    "java": ("pom.xml", "build.gradle", "build.gradle.kts"),
    "php": ("composer.json",),
    "ruby": ("Gemfile",),
    "dotnet": ("*.csproj", "*.sln"),
    "docker": ("Dockerfile", "docker-compose.yml", "compose.yaml"),
}


@dataclass
class ProjectInfo:
    root: Path
    kinds: list[str] = field(default_factory=list)
    name: str = ""
    version: str = ""
    description: str = ""
    dependencies: list[str] = field(default_factory=list)
    dev_dependencies: list[str] = field(default_factory=list)
    commands: dict[str, str] = field(default_factory=dict)
    entry_points: list[str] = field(default_factory=list)
    tests_dir: str = ""
    readme: str = ""

    def summary(self) -> str:
        lines = [f"Проект: {self.name or self.root.name} ({', '.join(self.kinds) or 'неизвестный тип'})"]
        if self.description:
            lines.append(self.description[:200])
        if self.dependencies:
            lines.append("Зависимости: " + ", ".join(self.dependencies[:20]))
        if self.commands:
            lines.append("Команды: " + "; ".join(f"{k} → {v}" for k, v in self.commands.items()))
        if self.tests_dir:
            lines.append(f"Тесты: {self.tests_dir}")
        return "\n".join(lines)


def detect(workspace: str | Path = ".") -> ProjectInfo:
    root = Path(workspace).resolve()
    info = ProjectInfo(root=root)

    for kind, markers in MARKERS.items():
        for marker in markers:
            if list(root.glob(marker)):
                info.kinds.append(kind)
                break

    if (root / "pyproject.toml").is_file():
        _read_pyproject(root / "pyproject.toml", info)
    if (root / "package.json").is_file():
        _read_package_json(root / "package.json", info)
    if (root / "Cargo.toml").is_file():
        info.commands.setdefault("тесты", "cargo test")
    if (root / "go.mod").is_file():
        info.commands.setdefault("тесты", "go test ./...")

    for name in ("tests", "test", "spec", "__tests__"):
        if (root / name).is_dir():
            info.tests_dir = name
            break

    for name in ("README.md", "README.rst", "README.txt", "readme.md"):
        candidate = root / name
        if candidate.is_file():
            info.readme = name
            break

    if "python" in info.kinds:
        info.commands.setdefault("тесты", "pytest -q")
        if info.entry_points:
            info.commands.setdefault("запуск", info.entry_points[0])
        elif (root / "main.py").is_file():
            info.commands.setdefault("запуск", "python main.py")
        info.commands.setdefault("линтер", "ruff check .")
    if "node" in info.kinds:
        info.commands.setdefault("тесты", "npm test")
        info.commands.setdefault("запуск", "npm run dev")
    if "docker" in info.kinds:
        info.commands.setdefault("контейнер", "docker compose up --build")

    return info


def _read_pyproject(path: Path, info: ProjectInfo) -> None:
    if tomllib is None:          # Python 3.10 без tomli — читаем хотя бы имя файла
        info.name = info.name or path.parent.name
        return
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return
    project = data.get("project") or {}
    info.name = project.get("name", info.name)
    info.version = str(project.get("version", info.version))
    info.description = project.get("description", info.description)
    for dep in project.get("dependencies") or []:
        info.dependencies.append(_clean_req(dep))
    for group in (project.get("optional-dependencies") or {}).values():
        for dep in group or []:
            info.dev_dependencies.append(_clean_req(dep))

    scripts = project.get("scripts") or {}
    for name in list(scripts)[:5]:
        info.entry_points.append(name)
        info.commands.setdefault(f"консольная команда {name}", name)

    tool = data.get("tool") or {}
    if (tool.get("pytest") or {}).get("testpaths"):
        info.commands.setdefault("тесты", "pytest -q")
    if tool.get("ruff"):
        info.commands.setdefault("линтер", "ruff check .")
    if tool.get("poetry"):
        info.commands.setdefault("установка", "poetry install")


def _read_package_json(path: Path, info: ProjectInfo) -> None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    info.name = info.name or data.get("name", "")
    info.version = info.version or str(data.get("version", ""))
    info.description = info.description or data.get("description", "")
    info.dependencies += list((data.get("dependencies") or {}).keys())[:20]
    info.dev_dependencies += list((data.get("devDependencies") or {}).keys())[:20]
    for script in ("test", "dev", "start", "build", "lint"):
        if script in (data.get("scripts") or {}):
            info.commands.setdefault(f"npm run {script}", str(data["scripts"][script])[:120])
    if (data.get("bin") or data.get("main")):
        info.entry_points.append(str(data.get("main") or list(data["bin"])[0]))


def _clean_req(requirement: str) -> str:
    for separator in ("==", ">=", "<=", "~=", ">", "<", " ["):
        requirement = requirement.split(separator)[0]
    return requirement.strip()


def stack_line(workspace: str | Path = ".") -> str:
    info = detect(workspace)
    parts = [f"тип: {', '.join(info.kinds) or 'не определён'}"]
    if info.name:
        parts.append(f"имя: {info.name}")
    if info.version:
        parts.append(f"версия: {info.version}")
    if info.commands.get("тесты"):
        parts.append(f"тесты: {info.commands['тесты']}")
    return " · ".join(parts)


__all__ = ["MARKERS", "ProjectInfo", "detect", "stack_line"]
