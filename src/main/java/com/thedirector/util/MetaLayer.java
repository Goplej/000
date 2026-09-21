package com.thedirector.util;

import com.thedirector.Config;
import com.thedirector.TheDirector;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import net.minecraft.server.level.ServerPlayer;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;

/**
 * Мета-слой.
 *
 * <p>Мод никогда не объясняет себя игроку, но при включённой опции {@code enableMetaEvents}
 * ведёт собственный журнал: {@code logs/thedirector_meta.log}. Это единственное место,
 * где режиссёр говорит прямо.</p>
 *
 * <p>Клиентская часть мета-слоя (скриншоты в момент крупных событий) живёт в
 * {@code com.thedirector.client.ClientMetaHandler} и реагирует на пакеты.</p>
 */
public final class MetaLayer {

    private static final DateTimeFormatter FORMAT = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss");

    /** Записать событие в журнал режиссёра. */
    public static void record(ServerPlayer player, DirectorEvent event, PlayerMemory memory) {
        if (!Config.metaEvents()) {
            return;
        }
        write(String.format("[%s] player=%s act=%d day=%d safety=%d dread=%d event=%s",
                ZonedDateTime.now().format(FORMAT),
                player.getGameProfile().getName(),
                memory.getCurrentAct(),
                memory.getDaysInWorld(),
                memory.getSafetyLevel(),
                memory.getDreadLevel(),
                event.id()));
    }

    /** Записать переход в новый акт. */
    public static void recordAct(ServerPlayer player, Act act, PlayerMemory memory) {
        if (!Config.metaEvents()) {
            return;
        }
        write(String.format("[%s] player=%s ACT_CHANGED act=%d (%s) day=%d",
                ZonedDateTime.now().format(FORMAT),
                player.getGameProfile().getName(),
                act.number(),
                act.label(),
                memory.getDaysInWorld()));
    }

    /** Записать произвольную строку (например, вход/выход из сна). */
    public static void note(ServerPlayer player, String text) {
        if (!Config.metaEvents()) {
            return;
        }
        write(String.format("[%s] player=%s %s",
                ZonedDateTime.now().format(FORMAT),
                player.getGameProfile().getName(),
                text));
    }

    private static void write(String line) {
        try {
            Path path = Path.of("logs", "thedirector_meta.log").toAbsolutePath();
            Files.createDirectories(path.getParent());
            Files.writeString(path, line + System.lineSeparator(), StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException | RuntimeException exception) {
            TheDirector.LOGGER.warn("[The Director] Не удалось записать мета-журнал: {}", exception.getMessage());
        }
    }

    private MetaLayer() {
    }
}
