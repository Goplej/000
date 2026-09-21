package com.thedirector.director;

import com.thedirector.Config;
import com.thedirector.TheDirector;
import com.thedirector.dream.DreamDimension;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.MetaLayer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;

/**
 * Главный режиссёр.
 *
 * <p>Вызывается раз в {@code eventFrequency} тиков (по умолчанию 100) для каждого игрока
 * и решает три вопроса: какой акт идёт, пора ли бить и каким событием.</p>
 *
 * <p>Базовые правила (из концепции мода):</p>
 * <ul>
 *   <li>{@code safetyLevel > 15 && dreadLevel < 5} — игрок расслабился, можно запускать крупное событие;</li>
 *   <li>{@code dreadLevel > 15} — тишина: режиссёр не добивает, он даёт страху улечься;</li>
 *   <li>акт 4 — события постоянны, "безопасных мест" больше нет.</li>
 * </ul>
 *
 * <p>Важно: режиссёр действует только на сервере. Клиент получает лишь звуки,
 * визуальные пакеты и изменения мира.</p>
 */
public final class Director {

    /** Тишина длиннее этого времени (в тиках) считается "безопасностью" и повышает safetyLevel. */
    private static final long SILENCE_FOR_SAFETY = 20L * 45L;

    /** После очень долгой тишины страх начинает спадать. */
    private static final long SILENCE_FOR_CALM = 20L * 120L;

    private Director() {
    }

    /** Точка входа: вызывается из обработчика PlayerTickEvent для серверного игрока. */
    public static void onPlayerTick(ServerPlayer player, PlayerMemory memory) {
        if (player.tickCount % Math.max(20, Config.eventFrequency()) != 0) {
            return;
        }
        ServerLevel level = player.serverLevel();
        long now = level.getGameTime();

        // --- 1. Учёт тишины: она работает в обе стороны ---
        long silence = memory.getLastEventTick() == 0L ? SILENCE_FOR_SAFETY : now - memory.getLastEventTick();
        if (silence > SILENCE_FOR_SAFETY) {
            memory.addSafety(1);
        }
        if (silence > SILENCE_FOR_CALM) {
            memory.addDread(-1);
        }

        // --- 2. Игрок под наблюдением: режиссёр всегда знает, где он ---
        memory.setLastSeenPos(player.blockPosition());

        // --- 3. Акт ---
        Act act = updateAct(player, memory);

        // --- 4. Сон важнее обычной режиссуры ---
        if (DreamDimension.isDream(level)) {
            DreamDimension.directDream(player, memory, act);
            return;
        }

        // --- 5. Добивать нельзя: страх выше 15 означает тишину ---
        if (memory.getDreadLevel() > 15) {
            return;
        }

        // --- 6. Пауза после предыдущего удара ---
        long delay = memory.getNextEventDelay();
        if (delay > 0L) {
            memory.setNextEventDelay(delay - Config.eventFrequency());
            return;
        }

        // --- 7. Решение о вмешательстве ---
        boolean safetyWindow = memory.getSafetyLevel() > 15 && memory.getDreadLevel() < 5;
        boolean pressure = act == Act.FOUR;
        boolean baselineNoise = memory.getSafetyLevel() > 8
                && player.getRandom().nextFloat() < baselineChance(act);
        if (!safetyWindow && !pressure && !baselineNoise) {
            return;
        }

        DirectorEvent event = EventDirector.pick(player, memory, act);
        if (event == null) {
            return;
        }
        fire(event, player, level, memory);
    }

    /**
     * Немедленно выполнить конкретное событие, минуя все задержки.
     *
     * <p>Используется командой {@code /thedirector event <id>}: это единственный способ
     * проверить мод, не ожидая нужного акта и настроения режиссёра.</p>
     *
     * @return {@code true}, если событие было запущено
     */
    public static boolean fireEventNow(ServerPlayer player, PlayerMemory memory, DirectorEvent event) {
        if (event == null) {
            return false;
        }
        fire(event, player, player.serverLevel(), memory);
        return true;
    }

    /** Применить событие и обновить память. */
    private static void fire(DirectorEvent event, ServerPlayer player, ServerLevel level, PlayerMemory memory) {
        try {
            event.execute(player, level, memory);
        } catch (RuntimeException exception) {
            TheDirector.LOGGER.error("[The Director] Событие {} упало", event.id(), exception);
            return;
        }
        memory.addDread(event.dreadCost());
        memory.addSafety(-event.safetyCost());
        memory.setEncounterCount(memory.getEncounterCount() + 1);
        memory.pushHistory(event.id());
        memory.setLastEventTick(level.getGameTime());
        memory.setNextEventDelay(silenceAfter(memory, event));

        MetaLayer.record(player, event, memory);
        if (Config.logEvents()) {
            TheDirector.LOGGER.info("[The Director] {} | акт {} | safety {} | dread {} | событие {}",
                    player.getGameProfile().getName(), memory.getCurrentAct(),
                    memory.getSafetyLevel(), memory.getDreadLevel(), event.id());
        }
    }

    /** Сколько тиков режиссёр будет молчать после события. */
    private static long silenceAfter(PlayerMemory memory, DirectorEvent event) {
        int[] range = switch (Act.of(memory.getCurrentAct())) {
            case ONE -> new int[]{200, 480};
            case TWO -> new int[]{140, 340};
            case THREE -> new int[]{80, 220};
            case FOUR -> new int[]{30, 100};
        };
        return range[0] + (long) (Math.random() * Math.max(1, range[1] - range[0]));
    }

    /** Вероятность "фонового" события за один цикл опроса (не считая окна безопасности). */
    private static float baselineChance(Act act) {
        return switch (act) {
            case ONE -> 0.12F;
            case TWO -> 0.25F;
            case THREE -> 0.40F;
            case FOUR -> 1.0F;
        };
    }

    /** Обновить акт по количеству дней; переход в новый акт отмечается в логе. */
    private static Act updateAct(ServerPlayer player, PlayerMemory memory) {
        Act act = Act.ofDay(memory.getDaysInWorld());
        if (act.number() != memory.getCurrentAct()) {
            memory.setCurrentAct(act.number());
            memory.setActStrikes(0);
            TheDirector.LOGGER.info("[The Director] {} входит в {} (день {}, игрок {})",
                    "Протагонист", act.label(), memory.getDaysInWorld(), player.getGameProfile().getName());
            MetaLayer.recordAct(player, act, memory);
        }
        return act;
    }
}
