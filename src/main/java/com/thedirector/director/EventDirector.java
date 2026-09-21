package com.thedirector.director;

import com.thedirector.director.event.ChatMessageEvent;
import com.thedirector.director.event.DaylightThunderEvent;
import com.thedirector.director.event.FOVShrinkEvent;
import com.thedirector.director.event.FakeBlockEvent;
import com.thedirector.director.event.FakeCrashEvent;
import com.thedirector.director.event.LightsOutEvent;
import com.thedirector.director.event.LostItemReturnEvent;
import com.thedirector.director.event.MovedChestEvent;
import com.thedirector.director.event.StepOnRoofEvent;
import com.thedirector.director.event.TorchExtinguishEvent;
import com.thedirector.director.event.WatcherEvent;
import com.thedirector.director.event.WhisperEvent;
import com.thedirector.director.event.WindowTitleEvent;
import com.thedirector.memory.PlayerMemory;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * Каталог событий и выбор следующего.
 *
 * <p>Режиссёр никогда не повторяет одно и то же два раза подряд и старается не
 * использовать событие, которое игрок видел в последних {@value #RECENT_LIMIT} записях
 * истории: привыкание убивает страх.</p>
 */
public final class EventDirector {

    /** Сколько последних событий считаются "свежими" (их не выбираем). */
    private static final int RECENT_LIMIT = 3;

    private static final List<DirectorEvent> EVENTS = new ArrayList<>();
    private static final Random RANDOM = new Random();

    static {
        // Порядок не важен: выбор идёт по весам.
        register(new WhisperEvent());
        register(new FakeBlockEvent());
        register(new WindowTitleEvent());
        register(new ChatMessageEvent());
        register(new TorchExtinguishEvent());
        register(new MovedChestEvent());
        register(new FOVShrinkEvent());
        register(new StepOnRoofEvent());
        register(new LostItemReturnEvent());
        register(new LightsOutEvent());
        register(new DaylightThunderEvent());
        register(new WatcherEvent());
        register(new FakeCrashEvent());
    }

    /** Зарегистрировать событие (доступно и для внешних расширений мода). */
    public static void register(DirectorEvent event) {
        EVENTS.add(event);
    }

    /** Все известные события. */
    public static List<DirectorEvent> all() {
        return List.copyOf(EVENTS);
    }

    /**
     * Выбрать событие для игрока: подходящее по акту, доступное по контексту,
     * не повторяющееся и с учётом веса.
     */
    public static DirectorEvent pick(ServerPlayer player, PlayerMemory memory, Act act) {
        ServerLevel level = player.serverLevel();
        List<String> recent = recentHistory(memory);

        List<DirectorEvent> candidates = new ArrayList<>();
        int totalWeight = 0;
        for (DirectorEvent event : EVENTS) {
            if (event.minAct().number() > act.number()) {
                continue;
            }
            if (recent.contains(event.id())) {
                continue;
            }
            if (!event.canRun(player, memory, level)) {
                continue;
            }
            candidates.add(event);
            totalWeight += Math.max(1, event.weight());
        }
        if (candidates.isEmpty()) {
            return null;
        }

        int roll = RANDOM.nextInt(totalWeight);
        for (DirectorEvent event : candidates) {
            roll -= Math.max(1, event.weight());
            if (roll < 0) {
                return event;
            }
        }
        return candidates.get(candidates.size() - 1);
    }

    /** Последние id событий из истории игрока. */
    private static List<String> recentHistory(PlayerMemory memory) {
        List<String> history = memory.getEventsHistory();
        int from = Math.max(0, history.size() - RECENT_LIMIT);
        return history.subList(from, history.size());
    }

    private EventDirector() {
    }
}
