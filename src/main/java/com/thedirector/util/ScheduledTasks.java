package com.thedirector.util;

import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.Level;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

/**
 * Планировщик отложенных задач на стороне сервера.
 *
 * <p>Нужен для "событий с последствием": тушение факела возвращает огонь через 3 секунды,
 * ложный краш закрывается через 7 секунд, шаги на крыше повторяются серией.</p>
 */
public final class ScheduledTasks {

    /** Максимум задач в одном тике измерения — защита от просадок TPS. */
    private static final int MAX_TASKS_PER_TICK = 512;

    private static final List<Task> TASKS = new ArrayList<>();

    private static final class Task {
        private final ResourceKey<Level> dimension;
        private int delay;
        private final Runnable action;

        private Task(ResourceKey<Level> dimension, int delay, Runnable action) {
            this.dimension = dimension;
            this.delay = delay;
            this.action = action;
        }
    }

    /** Запланировать выполнение через {@code delayTicks} тиков этого измерения. */
    public static void schedule(Level level, int delayTicks, Runnable action) {
        TASKS.add(new Task(level.dimension(), Math.max(1, delayTicks), action));
    }

    /** Вызывается из обработчика TickEvent.LevelTickEvent. */
    public static void tick(ServerLevel level) {
        if (TASKS.isEmpty()) {
            return;
        }
        int executed = 0;
        Iterator<Task> iterator = TASKS.iterator();
        while (iterator.hasNext() && executed < MAX_TASKS_PER_TICK) {
            Task task = iterator.next();
            if (!task.dimension.equals(level.dimension())) {
                continue;
            }
            task.delay--;
            if (task.delay <= 0) {
                iterator.remove();
                executed++;
                try {
                    task.action.run();
                } catch (RuntimeException exception) {
                    // Одно сломанное событие не должно ломать весь мод
                    com.thedirector.TheDirector.LOGGER.error("[The Director] Задача планировщика упала", exception);
                }
            }
        }
    }

    /** Полная очистка (остановка сервера, выгрузка мира). */
    public static void clear() {
        TASKS.clear();
    }

    /** Сколько задач в очереди (для отладки). */
    public static int size() {
        return TASKS.size();
    }

    private ScheduledTasks() {
    }
}
