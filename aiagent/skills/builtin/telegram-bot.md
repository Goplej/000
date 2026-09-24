---
name: telegram-bot
description: Телеграм-бот на aiogram 3: команды, кнопки, состояния, запуск
tags: telegram aiogram бот python
---

# Телеграм-бот (aiogram 3)

## 1. Подготовка
1. Уточни у пользователя токен бота (получить: @BotFather → /newbot). Токен — в переменную окружения, не в код.
2. Проверь версию: `pip show aiogram` (нужна 3.x — API отличается от 2.x).
3. Структура:
```
bot/
  __init__.py
  main.py        — запуск, диспетчер, polling
  handlers.py    — роутеры и обработчики
  keyboards.py   — клавиатуры
  states.py      — FSM-состояния
  settings.py    — загрузка токена из .env
  .env.example   — шаблон без секретов
```

## 2. Код (особенности aiogram 3)
- `Bot(token=...)`, `Dispatcher()`, роутеры через `@router.message(Command("start"))`.
- Запуск: `asyncio.run(dp.start_polling(bot))`.
- Клавиатуры: `InlineKeyboardBuilder` / `ReplyKeyboardBuilder`, ответ `await message.answer(..., reply_markup=kb.as_markup())`.
- FSM: `StatesGroup` + `state=` в декораторе, `await state.set_state(...)` / `state.clear()`.
- Долгие операции — `await bot.send_chat_action(chat_id, "typing")` и отдельная задача.
- Обработчик ошибок: `@dp.error()` с логом и понятным сообщением пользователю.

## 3. Проверка
1. Синтаксис и импорт: `python -c "import bot.main"` (с тестовым токеном из переменной окружения).
2. Юнит-тест обработчика без сети: подменить `message.answer` моком.
3. Если пользователь дал токен — запустить на 10–15 секунд (`shell_start`, затем `shell_output`),
   убедиться, что нет ошибок авторизации; остановить (`shell_kill`).
4. Никогда не печатать токен в вывод и логи.

## 4. Итог
- README: как установить зависимости, где взять токен, как запустить.
- Список команд и что делает каждая.
