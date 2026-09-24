# AI Agent Studio

**ИИ-агент уровня Claude Code на твоём компьютере.** Не «обёртка над чатом», а полноценный
автономный агент: читает и пишет файлы, запускает команды и тесты, ищет в интернете, работает
с git и GitHub, ведёт план работ, помнит проект и откатывает свои изменения.

Модель не обучается локально — агент работает через **облачные API** (Claude, GPT, Gemini,
DeepSeek, Grok, Mistral) или с **локальной Ollama**, если хочется без интернета.
Весь код агента — только стандартная библиотека Python, поэтому ставится за секунды и не тянет
десятки зависимостей.

```bash
aia "сделай телеграм-бота на aiogram со SQLite и тестами"
```

---

## Возможности

| Что умеет | Как это выглядит |
|---|---|
| **Делает файлы** | создаёт, читает и правит код точечными правками (`edit_file`, `apply_patch`) |
| **Запускает и проверяет** | терминал, тесты, линтеры, фоновые процессы (сервер, watcher) |
| **Ищет информацию** | веб-поиск без API-ключей, чтение страниц, Wikipedia |
| **Общается** | интерактивный чат с историей, стримингом и слэш-командами |
| **Планирует** | план работ (`todo`), под-агенты для подзадач, самопроверка результата |
| **Помнит** | память проекта, инструкции (`AIAGENT.md`), сессии, восстановление после перезапуска |
| **Не ломает** | подтверждения опасных действий, откат изменений (`/undo`), хуки, режимы |
| **Работает с git/GitHub** | статус и diff, коммиты, issues и pull request'ы |
| **Расширяется** | MCP-серверы, навыки (методики), свои инструменты |

Проверено на живом запуске: **45 инструментов**, **103 автотеста**, веб-интерфейс с SSE-стримингом.

---

## Быстрый старт

Нужен Python 3.10+ (внешние библиотеки не требуются).

```bash
git clone <репозиторий> && cd 000
python3 -m aiagent.cli keys set anthropic sk-ant-...   # или другой провайдер
python3 -m aiagent.cli                                 # чат в текущем проекте
```

Если команда `aia` ещё не установлена как скрипт, используй `python3 -m aiagent.cli …`
или установи пакет: `pip install -e .` (появится команда `aia`).

### Первая задача

```bash
aia                                 # интерактивный режим: пишешь задачу — агент делает
aia "перепиши скрипт на argparse и добавь тесты"
aia --provider anthropic --model claude-sonnet-4-5-20250929 "добавь логирование в api.py"
aia --mode yolo "прогони тесты и почини падения"   # без подтверждений
aia --provider mock "покажи демо"                  # демо-режим: модель не нужна
```

---

## Провайдеры и модели

| Провайдер | Ключ (переменная окружения) | Модель по умолчанию |
|---|---|---|
| `anthropic` | `ANTHROPIC_API_KEY` | `claude-sonnet-4-5-20250929` |
| `openai` | `OPENAI_API_KEY` | `gpt-4o` |
| `google` | `GOOGLE_API_KEY` / `GEMINI_API_KEY` | `gemini-2.5-flash` |
| `deepseek` | `DEEPSEEK_API_KEY` | `deepseek-chat` |
| `groq` | `GROQ_API_KEY` | llama-3.3-70b-versatile |
| `mistral` | `MISTRAL_API_KEY` | mistral-large-latest |
| `openrouter` | `OPENROUTER_API_KEY` | любая модель OpenRouter |
| `ollama` | не нужен | `qwen2.5-coder:7b` |
| `local` | не нужен | свой OpenAI-совместимый сервер |
| `mock` | не нужен | демо-режим без модели |

Ключи можно не держать в переменных окружения: `aia keys set anthropic <ключ>` положит их в
`~/.aiagent/keys.json` с правами `600`.

```bash
aia models                 # что доступно и что настроено
aia doctor                 # ключи, интернет, железо, Ollama
aia pull qwen2.5-coder:7b  # локальная модель через Ollama
```

---

## Как это работает

```
задача ──► системный промпт (проект, память, план, навыки, MCP)
              │
              ├──► модель (Anthropic / OpenAI / Gemini / …)
              │        └─ ответ: текст + вызовы инструментов
              │
              ├──► инструменты: файлы, терминал, поиск, git, GitHub…
              │        └─ подтверждение → снимок → выполнение → лог
              │
              └──► результат возвращается модели ──► следующий шаг … ──► итог
```

Цикл агента (`aiagent/core/agent.py`): автосжатие контекста → запрос к модели → выполнение
инструментов (несколько безопасных — параллельно) → проверка «не ленится ли модель» →
следующий шаг. Максимум шагов и все лимиты настраиваются.

### Режимы разрешений

| Режим | Что разрешено |
|---|---|
| `ask` | спрашивает про каждое изменение |
| `edits` | правки файлов свободно, команды — с подтверждением |
| `auto` | автономно внутри проекта (по умолчанию) |
| `plan` | ничего не меняет: только изучает и предлагает план |
| `yolo` | всё без подтверждений (для контейнера/песочницы) |

---

## Инструменты (45)

- **Файлы:** `read_file`, `write_file`, `edit_file`, `multi_edit`, `apply_patch`, `preview_patch`,
  `replace_in_files`, `list_dir`, `glob_search`, `grep_search`, `delete_file`, `move_file`, `file_info`
- **Терминал:** `run_shell`, `run_code`, фоновые `shell_start` / `shell_output` / `shell_list` / `shell_kill`
- **Интернет:** `web_search`, `fetch_url`, `http_request`, `wikipedia`, `site_links`
- **Планирование:** `todo_write`, `todo_read`, `update_plan`, `task` (под-агент), `ask_user`, `finish`
- **Память:** `memory_write`, `memory_read`, `project_rules_write`, `project_rules_read`, `gitignore_add`
- **GitHub:** `github_status`, `github_issues`, `github_create_issue`, `github_create_pr`, `github_repo_info`
- **Ноутбуки:** `notebook_read`, `notebook_edit`, `notebook_run`
- **Навыки:** `skills_list`, `read_skill`
- **MCP:** любые инструменты подключённых MCP-серверов (`mcp__сервер__инструмент`)

```bash
aia tools     # весь список с описаниями
```

### Под-агенты

Инструмент `task` запускает вложенного агента с урезанным набором инструментов:
`general`, `researcher`, `coder`, `reviewer`, `tester`, `planner`. Так большая задача
разбивается на части, и основной контекст не забивается мусором.

### Навыки (методики)

Готовые пошаговые инструкции: `rest-api`, `debug`, `code-review`, `git-workflow`, `tests`,
`docker-deploy`, `telegram-bot`, `web-scraping`. Агент видит каталог навыков в промпте и
подгружает нужный через `read_skill`. Свои навыки кладутся в `~/.aiagent/skills/` (личные)
или `<проект>/.aiagent/skills/` (для команды).

### MCP

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/home/user/project"]
    },
    "мой-api": { "url": "http://127.0.0.1:8080/mcp" }
  }
}
```

Файл `.mcp.json` в проекте или `~/.aiagent/mcp.json`. Проверить: `aia mcp`.

---

## Веб-интерфейс и API

```bash
aia serve                  # http://127.0.0.1:8787
aia serve --port 9000 --host 0.0.0.0 --provider anthropic
```

Тёмный чат-интерфейс: стриминг ответа, карточки инструментов, Markdown, модалка подтверждений,
история сессий, сайдбар. Всё — статика из `aiagent/web/`, без сборщиков и npm.

HTTP API (то же, что использует интерфейс):

| Метод | Адрес | Назначение |
|---|---|---|
| GET | `/api/health` | состояние агента, провайдер, число инструментов |
| GET | `/api/tools`, `/api/sessions`, `/api/history`, `/api/mcp` | справочная информация |
| POST | `/api/chat` | задача; ответ — поток SSE (`start`, `step`, `tool`, `text`, `done`) |
| POST | `/api/stop`, `/api/undo`, `/api/confirm`, `/api/reset` | управление выполнением |

---

## Команды CLI

```
aia                       интерактивный режим (по умолчанию)
aia "задача"              выполнить задачу и выйти
aia run | chat            явные формы запуска
aia doctor                окружение, ключи, интернет, железо
aia keys set|list         ключи API
aia models | pull         модели у провайдеров / скачать локальную
aia serve                 HTTP API + веб-интерфейс
aia init [папка]          подготовить проект (AIAGENT.md, .aiagent.json)
aia sessions | undo [N]   сессии и откат изменений
aia mcp | tools | config  MCP, инструменты, настройки
```

В чате доступны слэш-команды: `/help`, `/model`, `/provider`, `/mode`, `/context`, `/compact`,
`/system`, `/stats`, `/undo`, `/history`, `/todo`, `/notes`, `/rules`, `/keys`, `/save`, `/load`,
`/sessions`, `/edit`, `/shell`, `/diff`, `/reset`, `/exit`.

---

## Настройки и файлы состояния

Порядок применения: значения по умолчанию → `~/.aiagent/config.json` → `.aiagent.json` в проекте
→ переменные окружения → флаги командной строки.

```
~/.aiagent/
├── config.json      настройки
├── keys.json        ключи API (права 600)
├── mcp.json         MCP-серверы
├── logs/            журналы
└── projects/<проект>/
    ├── sessions/    сохранённые диалоги
    ├── checkpoints/ снимки файлов для отката
    ├── memory.md    память проекта
    └── todo.json    текущий план
```

Пояснения к проекту живут в `AIAGENT.md` (или `.aiagent/AIAGENT.md`, `AGENT.md`) —
агент читает их при каждом запуске и слушается.

Основные настройки: `max_steps`, `mode`, `temperature`, `max_tokens`, `compact_at_ratio`,
`tool_result_max_chars`, `parallel_tools`, `hooks`, `web_host`, `web_port`.

```bash
aia config --show
aia config --set mode=auto --set max_steps=80
```

---

## Безопасность

- **Режимы разрешений** и подтверждение опасных команд прямо в чате (или в веб-модалке).
- **Блок-лист** разрушительных команд (`rm -rf /`, `mkfs`, `dd`, форк-бомбы и т.п.).
- **Откат**: перед каждой записью делается снимок, `/undo 3` вернёт три последних правки.
- **Секреты** вырезаются из логов и не попадают в контекст модели.
- **Хуки** `pre_tool` / `post_tool` / `stop` — свои проверки и запреты (код возврата 2 блокирует вызов).

---

## Архитектура

```
aiagent/
├── cli.py            командная строка (aia)
├── commands.py       слэш-команды чата
├── config.py         настройки, режимы, правила разрешений
├── prompts.py        сборка системного промпта
├── session.py        сохранение сессий
├── paths.py          каталоги состояния
├── errors.py         понятные ошибки
├── hardware.py       определение железа и рекомендации по локальным моделям
├── core/             agent, parser, context, permissions, hooks, undo, subagent, types
├── providers/        anthropic, openai, google, ollama, mock, …
├── tools/            files, shell, web, search, tasks, memory, patch, notebook, github, skills
├── integrations/     git, project, github
├── skills/builtin/   встроенные методики
├── mcp/              клиент и реестр MCP
├── server/           HTTP API + SSE
├── ui/               консоль, рендер, события
└── web/              интерфейс (HTML/CSS/JS)
tests/                103 автотеста (pytest, без сети и ключей)
```

Тесты запускаются на демо-провайдере, поэтому не требуют ни ключей, ни интернета:

```bash
python -m pytest tests/ -q
```

---

## Ограничения

- Агент **не обучает модель**: качество ответов определяется выбранной моделью (Claude, GPT и др.).
- Локальные модели через Ollama работают, но на слабой видеокарте (4 ГБ) им тесно — для сложных
  задач лучше облачный провайдер.
- Веб-поиск использует публичные страницы (DuckDuckGo/SearX и т.п.): при блокировке выдачи
  возможностей меньше, чем у платного поискового API.

---

## Лицензия

MIT — пользуйся, изменяй, публикуй.
