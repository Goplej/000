---
name: web-scraping
description: Собрать данные с сайта: разведка, парсинг, сохранение, вежливость
tags: парсинг scraping requests html json
---

# Сбор данных с сайтов

## 1. Разведка
1. Открой страницу: `fetch_url` (получишь текст без тегов) — оцени структуру и объём.
2. Найди реальные данные: часто они лежат в JSON внутри страницы
   (`__NEXT_DATA__`, `window.__data`), в `application/ld+json` или в API вида `/api/...`.
   Инспектируй `http_request` к найденному эндпоинту — это надёжнее парсинга HTML.
3. Проверь правила сайта: `fetch_url https://сайт/robots.txt`, условия использования.
4. Оцени пагинацию и лимиты (сколько страниц, есть ли rate-limit).

## 2. Код
```python
import json, time, urllib.request

UA = {"User-Agent": "Mozilla/5.0 (research script; contact@example.com)"}

def get(url, tries=3):
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=20) as r:
                return r.read().decode("utf-8", "replace")
        except Exception as e:
            if attempt == tries - 1:
                raise
            time.sleep(2 ** attempt)
```
- Данные — в `list[dict]`, сохранять сразу: `json.dump(..., ensure_ascii=False, indent=2)`.
- Пауза 1–2 секунды между запросами; не более 3 повторов; не обходить блокировки и капчи.
- Парсинг HTML — `html.parser` из стандартной библиотеки или `selectolax`/`beautifulsoup4`,
  если пользователь разрешил установку.
- Логировать прогресс: `стр. 3/20 — 47 записей`.

## 3. Проверка
1. Прогони на 1–2 страницах, посмотри первые записи (`run_code` с печатью среза).
2. Проверь краевые случаи: пустая страница, отсутствующее поле (используй `.get()`).
3. Только после этого — полный сбор; сохрани результат рядом с кодом.

## 4. Итог
- Скажи, сколько записей собрано, куда сохранено, какие поля доступны.
- Отметь ограничения: сколько страниц прошло, что осталось за кадром.
