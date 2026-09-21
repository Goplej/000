package com.thedirector.client;

import com.thedirector.Config;
import com.thedirector.TheDirector;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;

/**
 * Клиентская часть мета-слоя: журнал того, что видел игрок.
 *
 * <p>Пишется только при включённой опции {@code enableMetaEvents}: в отличие от
 * серверного журнала, здесь фиксируются эффекты (скриншоты, ложный краш, подмена
 * заголовка окна) — то, что происходит на машине игрока.</p>
 */
public final class ClientMetaHandler {

    private static final DateTimeFormatter FORMAT = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss");

    private ClientMetaHandler() {
    }

    /** Записать строку в клиентский мета-журнал. */
    public static void note(String text) {
        if (!Config.metaEvents()) {
            return;
        }
        try {
            Path path = Path.of("logs", "thedirector_meta_client.log").toAbsolutePath();
            Files.createDirectories(path.getParent());
            Files.writeString(path, "[" + ZonedDateTime.now().format(FORMAT) + "] " + text + System.lineSeparator(),
                    StandardCharsets.UTF_8, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException | RuntimeException exception) {
            TheDirector.LOGGER.debug("[The Director] Клиентский мета-журнал недоступен: {}", exception.getMessage());
        }
    }
}
