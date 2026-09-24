---
name: docker-deploy
description: Упаковать проект в Docker и подготовить запуск на сервере
tags: docker deploy devops nginx
---

# Docker и деплой

## 1. Разведка
1. Определи стек: версия Python/Node, системные зависимости, порт приложения.
2. Найди команды запуска (`README`, `Makefile`, `pyproject`): что именно исполнять в контейнере.
3. Проверь, есть ли уже `Dockerfile`, `.dockerignore`, `docker-compose.yml`.

## 2. Dockerfile (Python-пример)
```dockerfile
FROM python:3.12-slim AS base
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .
RUN useradd -m app && chown -R app /app
USER app
EXPOSE 8000
HEALTHCHECK CMD python -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8000/health')"
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
```
Правила: не запускать под root, кэшировать слои (зависимости раньше кода),
не копировать `.env` и тесты в образ, использовать `-slim`/`-alpine` базовый образ.

## 3. .dockerignore
```
.git
.venv
__pycache__
*.pyc
.env
tests/
.aia/
```
Плюс всё, что не нужно в рантайме.

## 4. Compose и запуск
```yaml
services:
  app:
    build: .
    ports: ["8000:8000"]
    env_file: [.env]
    restart: unless-stopped
```
Проверка: `docker compose up --build -d`, затем `docker compose logs --tail=50 app`
и `curl -s localhost:8000/health`. Остановить: `docker compose down`.

## 5. Безопасность
- Секреты — только через переменные окружения/секреты, никогда не в образ.
- В README описать: обновление (`git pull && docker compose up --build -d`), бэкапы, где логи.
- Не выполнять деплой на реальный сервер без явного согласия пользователя.
