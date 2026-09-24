---
name: rest-api
description: Создать REST API на FastAPI с моделями, валидацией и тестами
tags: fastapi api backend python web
---

# Создание REST API

## 0. Разведка
1. `list_dir` и `read_file` — есть ли уже структура проекта, зависимости, стиль кода.
2. Посмотри `pyproject.toml` / `requirements.txt`: какая версия Python, есть ли FastAPI.
3. Спроси у пользователя только то, что нельзя выяснить из кода (например, есть ли БД).

## 1. Структура проекта
```
app/
  __init__.py
  main.py          — создание FastAPI-приложения, подключение роутеров
  api/
    __init__.py
    routes.py      — эндпоинты
  models.py        — Pydantic-схемы
  storage.py       — доступ к данным (в памяти, файл или БД)
  settings.py      — настройки через переменные окружения
tests/
  test_api.py
```

## 2. Код
- Pydantic-модели: отдельные схемы на создание (`ItemCreate`), обновление (`ItemUpdate`) и ответ (`Item`).
- Валидация — средствами Pydantic (`Field(min_length=... , gt=0)`), без ручных `if`.
- Каждый эндпоинт: короткая докстрока, `response_model`, коды ответов (`status_code=201` для создания, `404` для отсутствующего).
- Ошибки — через `HTTPException` с понятным русским текстом.
- Обработку исключений не глотать: логировать через `logging.getLogger(__name__)`.

## 3. Проверка (обязательно)
1. Установи зависимости: `pip install fastapi uvicorn pytest httpx` (или добавь в pyproject).
2. Тесты — `TestClient` из `fastapi.testclient`: успешный сценарий, валидация, 404, границы.
3. Прогони: `pytest -q`.
4. Запусти сервер в фоне (`run_shell` c `&` или `shell_start`) и проверь `curl`:
   - `curl -s localhost:8000/health`
   - `curl -s -X POST localhost:8000/items -H 'Content-Type: application/json' -d '{...}'`
5. Останови фоновый процесс (`shell_kill`).

## 4. Итог
- Обнови README: как запустить и как проверить.
- Перечисли созданные файлы и команды проверки в ответе.
