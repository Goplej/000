#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Генератор звуков мода "The Director".

Мод не тянет за собой сторонние ассеты: вся звуковая палитра синтезируется
программно (шум + резонансные фильтры + огибающие) и сохраняется в Ogg Vorbis —
единственный формат, который понимает Minecraft.

Запуск:  python3 tools/generate_sounds.py
Результат: src/main/resources/assets/thedirector/sounds/*.ogg

Список звуков и их смысл:
  whisper       — шёпот за спиной (слоговая структура из полосового шума)
  breath        — дыхание рядом
  roof_step     — шаг по крыше (низкий удар + гравий)
  step_echo     — собственный шаг со эхом (сон)
  void_ambient  — пустой гул сна (биения низких частот)
  static_noise  — помехи
  crash         — ложный краш (суб-бум + металлический звон)
  extinguish    — тушение огня
  wood_creak    — скрип дерева в доме
  reverse       — предмет работает наоборот (обратная огибающая)
  chest         — деревянный стук сундука
  thunder       — гром при ясном небе
"""

import os
import numpy as np
import soundfile as sf

SR = 44100
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "src", "main", "resources", "assets", "thedirector", "sounds")

rng = np.random.default_rng(20260921)


# --------------------------------------------------------------------- утилиты

def noise(seconds: float) -> np.ndarray:
    return rng.normal(0.0, 1.0, int(SR * seconds)).astype(np.float32)


def lowpass(x: np.ndarray, cutoff: float, order: int = 2) -> np.ndarray:
    """Простой однополюсный/двухполюсный фильтр низких частот."""
    alpha = 1.0 - np.exp(-2.0 * np.pi * cutoff / SR)
    y = np.zeros_like(x)
    acc = 0.0
    for _ in range(order):
        acc = 0.0
        for i in range(len(x)):
            acc += alpha * (x[i] - acc)
            y[i] = acc
        x = y.copy()
    return y


def highpass(x: np.ndarray, cutoff: float) -> np.ndarray:
    return (x - lowpass(x, cutoff)).astype(np.float32)


def bandpass(x: np.ndarray, low: float, high: float) -> np.ndarray:
    return highpass(lowpass(x, high), low)


def resonator(x: np.ndarray, freq: float, q: float, gain: float = 1.0) -> np.ndarray:
    """Резонансный фильтр (двухполюсник) — даёт "жестяной"/деревянный призвук."""
    w = 2.0 * np.pi * freq / SR
    r = np.exp(-w / (2.0 * q))
    a1 = 2.0 * r * np.cos(w)
    a2 = -r * r
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    for i in range(len(x)):
        cur = x[i] + a1 * y1 + a2 * y2
        y2 = y1
        y1 = cur
        y[i] = cur * (1.0 - r) * gain
    return y


def envelope(seconds: float, attack: float, decay: float, power: float = 1.0) -> np.ndarray:
    n = int(SR * seconds)
    a = max(1, int(SR * attack))
    d = max(1, n - a)
    env = np.concatenate([
        np.linspace(0.0, 1.0, a) ** 0.6,
        (np.linspace(1.0, 0.0, d) ** power)
    ])
    return env[:n].astype(np.float32)


def tone(freq: float, seconds: float, phase: float = 0.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    return np.sin(2.0 * np.pi * freq * t + phase).astype(np.float32)


def sweep(f0: float, f1: float, seconds: float) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    k = (f1 - f0) / max(seconds, 1e-6)
    phase = 2.0 * np.pi * (f0 * t + 0.5 * k * t * t)
    return np.sin(phase).astype(np.float32)


def normalize(x: np.ndarray, peak: float = 0.6) -> np.ndarray:
    m = float(np.max(np.abs(x))) if len(x) else 1.0
    return (x / m * peak).astype(np.float32) if m > 0 else x


def fade_edges(x: np.ndarray, ms: float = 8.0) -> np.ndarray:
    k = max(1, int(SR * ms / 1000.0))
    if len(x) < 2 * k:
        return x
    ramp = np.linspace(0.0, 1.0, k, dtype=np.float32)
    x[:k] *= ramp
    x[-k:] *= ramp[::-1]
    return x


def write(name: str, x: np.ndarray, peak: float = 0.6, quality: float = 0.35) -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, name + ".ogg")
    data = fade_edges(normalize(x, peak))
    sf.write(path, data, SR, format="OGG", subtype="VORBIS")
    print(f"  {name}.ogg  {os.path.getsize(path) / 1024.0:6.1f} КБ")


# --------------------------------------------------------------------- звуки

def whisper() -> np.ndarray:
    """Шёпот: полосовой шум, разбитый на слоги случайной длины."""
    total = 2.2
    base = bandpass(noise(total), 320.0, 3200.0)
    # "слоговая" структура: 7-11 коротких всплесков с паузами
    env = np.zeros_like(base)
    pos = int(SR * 0.05)
    while pos < len(base) - SR // 20:
        syllable = int(SR * (0.05 + rng.random() * 0.13))
        gap = int(SR * (0.02 + rng.random() * 0.09))
        end = min(len(base), pos + syllable)
        seg = np.linspace(0.0, 1.0, end - pos) ** 0.7
        seg *= (1.0 - np.linspace(0.0, 1.0, end - pos)) ** 0.5
        env[pos:end] = seg * (0.4 + rng.random() * 0.6)
        pos = end + gap
    voiced = base * env
    # модуляция "дыханием" — шёпот никогда не ровный
    wobble = 0.75 + 0.25 * np.sin(2.0 * np.pi * (3.0 + rng.random() * 4.0) * np.arange(len(voiced)) / SR)
    out = voiced * wobble
    out += 0.12 * lowpass(noise(total), 250.0) * envelope(total, 0.3, 0.6)  # низкий выдох под шёпотом
    return out


def breath() -> np.ndarray:
    total = 1.9
    air = lowpass(noise(total), 900.0, order=2)
    env = envelope(total, 0.55, 0.45, power=1.4)
    trem = 0.85 + 0.15 * np.sin(2.0 * np.pi * 1.3 * np.arange(len(air)) / SR)
    return air * env * trem


def roof_step() -> np.ndarray:
    total = 0.4
    thud = tone(72.0, total) * envelope(total, 0.004, 0.09, power=2.2) * 0.9
    gravel = lowpass(noise(0.09), 2600.0) * envelope(0.09, 0.002, 0.05, power=2.0)
    out = np.zeros(int(SR * total), dtype=np.float32)
    out[:len(thud)] += thud
    out[:len(gravel)] += gravel * 0.55
    # редкие "камешки"
    for _ in range(6):
        i = int(rng.random() * (len(out) - 400))
        out[i:i + 200] += (rng.normal(0.0, 0.25, 200) * np.linspace(1.0, 0.0, 200)).astype(np.float32)
    return out


def step_echo() -> np.ndarray:
    total = 0.95
    step = lowpass(noise(0.12), 1800.0) * envelope(0.12, 0.002, 0.06, power=1.8)
    out = np.zeros(int(SR * total), dtype=np.float32)
    for i, gain in enumerate((1.0, 0.55, 0.3, 0.16, 0.08)):
        delay = int(SR * 0.17 * i)
        if delay + len(step) > len(out):
            break
        out[delay:delay + len(step)] += step * gain
    out += 0.08 * lowpass(noise(total), 400.0)  # гул пустого пространства
    return out


def void_ambient() -> np.ndarray:
    total = 4.0
    a = tone(48.0, total) * 0.5
    b = tone(48.7, total) * 0.5          # биения ~0.7 Гц
    c = tone(96.4, total) * 0.18
    bed = lowpass(noise(total), 180.0, order=2) * 0.35
    lfo = 0.7 + 0.3 * np.sin(2.0 * np.pi * 0.12 * np.arange(int(SR * total)) / SR)
    return (a + b + c + bed) * lfo


def static_noise() -> np.ndarray:
    total = 1.3
    base = highpass(noise(total), 350.0)
    gate = (rng.random(int(SR * total)) > 0.35).astype(np.float32)
    gate = lowpass(gate, 90.0)          # сглаживаем "рваный" сигнал
    return base * gate


def crash() -> np.ndarray:
    total = 1.6
    boom = sweep(90.0, 28.0, 0.9) * envelope(0.9, 0.002, 0.35, power=2.0)
    burst = lowpass(noise(0.5), 3000.0) * envelope(0.5, 0.001, 0.2, power=2.6)
    ring = (resonator(noise(total), 880.0, 22.0) + resonator(noise(total), 1330.0, 18.0)) \
        * envelope(total, 0.001, 0.5, power=2.2)
    out = np.zeros(int(SR * total), dtype=np.float32)
    out[:len(boom)] += boom
    out[:len(burst)] += burst * 0.7
    out[:len(ring)] += ring * 0.25
    return out


def extinguish() -> np.ndarray:
    total = 0.75
    hiss = noise(total)
    # низкочастотный "пшшш": фильтр скользит вниз
    out = np.zeros_like(hiss)
    chunk = 512
    for start in range(0, len(hiss), chunk):
        progress = start / max(1, len(hiss))
        cutoff = 4000.0 * (1.0 - progress) + 260.0 * progress
        part = lowpass(hiss[start:start + chunk], cutoff, order=1)
        out[start:start + chunk] = part
    env = envelope(total, 0.01, 0.4, power=1.6)
    click = lowpass(noise(0.02), 1200.0) * envelope(0.02, 0.001, 0.008, power=1.5)
    out = out * env
    out[:len(click)] += click * 0.6
    return out


def wood_creak() -> np.ndarray:
    total = 1.5
    n = int(SR * total)
    t = np.arange(n) / SR
    # "трение-проскальзывание": медленный дрейф частоты + рваная амплитуда
    freq = 170.0 + 45.0 * np.sin(2.0 * np.pi * 0.7 * t) + 12.0 * np.sin(2.0 * np.pi * 5.3 * t)
    phase = 2.0 * np.pi * np.cumsum(freq) / SR
    body = (np.sin(phase) * 0.5 + np.sin(2.0 * phase) * 0.25).astype(np.float32)
    stutter = (rng.random(n) > 0.12).astype(np.float32)
    stutter = lowpass(stutter, 40.0)
    env = envelope(total, 0.25, 0.5, power=1.2)
    grit = lowpass(noise(total), 1500.0) * 0.2
    return (body * stutter + grit) * env


def reverse() -> np.ndarray:
    total = 0.9
    n = int(SR * total)
    t = np.arange(n) / SR
    freq = 700.0 * np.exp(-1.6 * t / total)      # тон уезжает вниз
    phase = 2.0 * np.pi * np.cumsum(freq) / SR
    bell = (np.sin(phase) + 0.5 * np.sin(2.01 * phase) + 0.3 * np.sin(3.02 * phase)).astype(np.float32)
    # обратная огибающая: долгий подъём и обрыв
    env = np.concatenate([np.linspace(0.0, 1.0, int(n * 0.92)) ** 1.5, np.zeros(n - int(n * 0.92))]).astype(np.float32)
    return bell * env


def chest() -> np.ndarray:
    total = 0.55
    knock = lowpass(noise(0.05), 900.0) * envelope(0.05, 0.001, 0.02, power=1.6)
    creak = wood_creak()[:int(SR * 0.3)] * 0.5
    out = np.zeros(int(SR * total), dtype=np.float32)
    out[:len(knock)] += knock
    out[:len(creak)] += creak
    return out


def thunder() -> np.ndarray:
    total = 3.0
    n = int(SR * total)
    rumble = lowpass(noise(total), 140.0, order=3)
    env = envelope(total, 0.05, 1.0, power=1.5)
    crack = highpass(noise(0.15), 1200.0) * envelope(0.15, 0.001, 0.05, power=2.0)
    sub = sweep(70.0, 35.0, 1.8) * envelope(1.8, 0.02, 0.7, power=1.8)
    out = rumble * env * 1.6
    out[:len(crack)] += crack * 0.5
    out[:len(sub)] += sub * 0.6
    # лёгкая реверберация: несколько затухающих копий
    for i in range(1, 5):
        d = int(SR * 0.22 * i)
        if d < n:
            out[d:] += out[:n - d] * (0.35 ** i)
    return out[:n]


SOUNDS = {
    "whisper": whisper,
    "breath": breath,
    "roof_step": roof_step,
    "step_echo": step_echo,
    "void_ambient": void_ambient,
    "static_noise": static_noise,
    "crash": crash,
    "extinguish": extinguish,
    "wood_creak": wood_creak,
    "reverse": reverse,
    "chest": chest,
    "thunder": thunder,
}

PEAKS = {
    "whisper": 0.45,
    "breath": 0.35,
    "roof_step": 0.55,
    "step_echo": 0.4,
    "void_ambient": 0.4,
    "static_noise": 0.4,
    "crash": 0.75,
    "extinguish": 0.45,
    "wood_creak": 0.4,
    "reverse": 0.4,
    "chest": 0.45,
    "thunder": 0.8,
}


def main() -> None:
    print("Генерация звуков The Director ->", OUT_DIR)
    for name, fn in SOUNDS.items():
        write(name, fn(), peak=PEAKS.get(name, 0.5))
    print("Готово.")


if __name__ == "__main__":
    main()
