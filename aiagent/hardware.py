"""Определение железа и рекомендации для локальных моделей (Ollama).

Даже если ты работаешь через облако, приятно знать, что потянет твоя машина:
сколько VRAM, какую модель взять, какой контекст выставить.
"""
from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Gpu:
    name: str = ""
    vram_gb: float = 0.0
    vendor: str = ""
    driver: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {"name": self.name, "vram_gb": self.vram_gb, "vendor": self.vendor,
                "driver": self.driver}


@dataclass
class Hardware:
    os_name: str = ""
    cpu: str = ""
    cpu_cores: int = 0
    ram_gb: float = 0.0
    gpus: list[Gpu] = field(default_factory=list)
    disk_free_gb: float = 0.0
    notes: list[str] = field(default_factory=list)

    @property
    def vram_gb(self) -> float:
        return max([g.vram_gb for g in self.gpus], default=0.0)

    @property
    def gpu_name(self) -> str:
        return self.gpus[0].name if self.gpus else ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "os_name": self.os_name, "cpu": self.cpu, "cpu_cores": self.cpu_cores,
            "ram_gb": self.ram_gb, "vram_gb": self.vram_gb, "gpu_name": self.gpu_name,
            "gpus": [g.to_dict() for g in self.gpus], "disk_free_gb": self.disk_free_gb,
            "notes": self.notes,
        }

    def summary(self) -> str:
        parts = [f"CPU {self.cpu_cores} потоков", f"RAM {self.ram_gb} ГБ"]
        if self.gpus:
            parts.append(f"{self.gpu_name} ({self.vram_gb} ГБ VRAM)")
        else:
            parts.append("без GPU")
        return " · ".join(parts)


def detect() -> Hardware:
    hw = Hardware(
        os_name=f"{platform.system()} {platform.release()}",
        cpu=platform.processor() or _cpu_name(),
        cpu_cores=os.cpu_count() or 0,
        ram_gb=round(_total_ram_gb(), 1),
        disk_free_gb=round(_disk_free(Path.cwd()), 1),
    )
    hw.gpus = detect_gpus()
    if not hw.gpus:
        hw.notes.append("GPU не найден: локальные 7B+ модели будут медленными — бери облако (--provider anthropic).")
    if hw.ram_gb and hw.ram_gb <= 9:
        hw.notes.append("RAM ≤ 9 ГБ: для 4 ГБ VRAM держи контекст ≤ 8k токенов.")
    return hw


def detect_gpus() -> list[Gpu]:
    gpus: list[Gpu] = []
    if shutil.which("nvidia-smi"):
        try:
            out = subprocess.run(
                ["nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=8, check=False,
            ).stdout
            for line in out.strip().splitlines():
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 2 and parts[1].isdigit():
                    gpus.append(Gpu(name=parts[0], vram_gb=round(int(parts[1]) / 1024, 1),
                                    vendor="nvidia", driver=parts[2] if len(parts) > 2 else ""))
        except (OSError, subprocess.SubprocessError):
            pass

    if platform.system() == "Windows" and not gpus:
        gpus.extend(_windows_gpus())

    if platform.system() == "Darwin" and not gpus:
        # Apple Silicon: память общая, берём половину как доступную для модели
        if platform.machine() in {"arm64", "aarch64"}:
            gpus.append(Gpu(name="Apple Silicon (unified memory)",
                            vram_gb=round(_total_ram_gb() * 0.6, 1), vendor="apple"))
    return gpus


def _windows_gpus() -> list[Gpu]:
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_VideoController | Select-Object Name,"
             "@{n='vram';e={[math]::Round($_.AdapterRAM/1GB,1)}} | ConvertTo-Csv -NoTypeInformation"],
            capture_output=True, text=True, timeout=20, check=False,
        ).stdout
        gpus = []
        for line in out.strip().splitlines()[1:]:
            parts = [p.strip('"') for p in line.split('","')]
            if parts and parts[0]:
                try:
                    vram = float(parts[1])
                except (IndexError, ValueError):
                    vram = 0.0
                gpus.append(Gpu(name=parts[0], vram_gb=vram, vendor="other"))
        return gpus
    except (OSError, subprocess.SubprocessError):
        return []


def _cpu_name() -> str:
    if platform.system() == "Linux":
        try:
            text = Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="ignore")
            match = re.search(r"model name\s*:\s*(.+)", text)
            if match:
                return match.group(1).strip()
        except OSError:
            pass
    return platform.machine()


def _total_ram_gb() -> float:
    try:
        if platform.system() == "Linux":
            for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
                if line.startswith("MemTotal:"):
                    return int(re.findall(r"\d+", line)[0]) / 1024 ** 2
        elif platform.system() == "Darwin":
            out = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True,
                                 text=True, timeout=8, check=False).stdout.strip()
            if out.isdigit():
                return int(out) / 1024 ** 3
        else:
            out = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"],
                capture_output=True, text=True, timeout=15, check=False).stdout
            nums = re.findall(r"\d{6,}", out)
            if nums:
                return int(nums[0]) / 1024 ** 3
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return 0.0


def _disk_free(path: Path) -> float:
    try:
        return shutil.disk_usage(path).free / 1024 ** 3
    except OSError:
        return 0.0


# --------------------------------------------------------------------------------------
# Рекомендации
# --------------------------------------------------------------------------------------
def recommend(hw: Hardware | None = None) -> dict[str, Any]:
    hw = hw or detect()
    vram, ram = hw.vram_gb, hw.ram_gb

    if vram >= 16:
        model, ctx, layers = "qwen2.5-coder:14b-instruct-q4_K_M", 32768, 99
    elif vram >= 10:
        model, ctx, layers = "qwen2.5-coder:7b-instruct-q5_K_M", 16384, 99
    elif vram >= 5.5:
        model, ctx, layers = "qwen2.5-coder:7b-instruct-q4_K_M", 8192, 99
    elif vram >= 3.0:
        model, ctx, layers = "qwen2.5-coder:7b-instruct-q3_K_M", 6144, None
    elif vram >= 1.5:
        model, ctx, layers = "qwen2.5-coder:3b-instruct-q4_K_M", 8192, 99
    else:
        model, ctx, layers = "qwen2.5-coder:1.5b-instruct-q4_K_M", 8192, None

    if ram and ram < 7:
        model, ctx = "qwen2.5-coder:1.5b-instruct-q4_K_M", 4096

    env = {
        "OLLAMA_KV_CACHE_TYPE": "q8_0",      # экономит половину VRAM на кэше контекста
        "OLLAMA_FLASH_ATTENTION": "1",
        "OLLAMA_MAX_LOADED_MODELS": "1",
        "OLLAMA_NUM_PARALLEL": "1",
        "OLLAMA_KEEP_ALIVE": "10m",
    }
    if vram and vram <= 4.5:
        env["OLLAMA_GPU_OVERHEAD"] = str(384 * 1024 * 1024)

    return {
        "model": model,
        "num_ctx": ctx,
        "num_gpu": layers,
        "env": env,
        "advice": [
            "Держи загруженной только одну модель — VRAM общий с браузером и играми.",
            f"Для твоего железа оптимальный контекст: {ctx} токенов.",
            "Проверить установку: aia doctor",
        ],
    }


def doctor_text(hw: Hardware | None = None) -> str:
    hw = hw or detect()
    rec = recommend(hw)
    lines = [
        "=== Железо ===",
        f"ОС:      {hw.os_name}",
        f"CPU:     {hw.cpu} ({hw.cpu_cores} потоков)",
        f"RAM:     {hw.ram_gb} ГБ",
        f"GPU:     {hw.gpu_name or 'нет'}    VRAM: {hw.vram_gb} ГБ",
        f"Диск:    свободно {hw.disk_free_gb} ГБ",
    ]
    lines += [f"  ! {note}" for note in hw.notes]
    lines += [
        "",
        "=== Для локальных моделей (Ollama) ===",
        f"Рекомендуемая модель: {rec['model']}",
        f"Контекст: {rec['num_ctx']} токенов",
        "",
        "Переменные окружения (Linux/macOS):",
        *[f"  export {k}={v}" for k, v in rec["env"].items()],
        "Windows PowerShell:",
        *[f"  [Environment]::SetEnvironmentVariable('{k}','{v}','User')" for k, v in rec["env"].items()],
        "",
        *[f"  • {a}" for a in rec["advice"]],
    ]
    return "\n".join(lines)


__all__ = ["Gpu", "Hardware", "detect", "detect_gpus", "doctor_text", "recommend"]
