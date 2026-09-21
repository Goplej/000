package com.thedirector;

import net.minecraftforge.common.ForgeConfigSpec;

/**
 * Конфигурация мода.
 *
 * <p>Файл генерируется автоматически: {@code config/thedirector-common.toml}.</p>
 */
public final class Config {

    public static final ForgeConfigSpec SPEC;

    public static final ForgeConfigSpec.BooleanValue ENABLE_META_EVENTS;
    public static final ForgeConfigSpec.BooleanValue ENABLE_META_SCREENSHOTS;
    public static final ForgeConfigSpec.BooleanValue ENABLE_DREAM_DIMENSION;
    public static final ForgeConfigSpec.DoubleValue DREAM_CHANCE;
    public static final ForgeConfigSpec.IntValue EVENT_FREQUENCY;
    public static final ForgeConfigSpec.IntValue DREAM_DURATION_MINUTES;
    public static final ForgeConfigSpec.BooleanValue ENABLE_TORCH_FLICKER;
    public static final ForgeConfigSpec.IntValue ACT_DAY_SCALE;
    public static final ForgeConfigSpec.BooleanValue LOG_EVENTS;

    static {
        ForgeConfigSpec.Builder builder = new ForgeConfigSpec.Builder();

        builder.comment("Мета-слой: внешние следы присутствия режиссёра (логи, скриншоты).",
                        "Мод не объясняет себя игроку, но при включении этой опции ведёт свои записи.",
                        "Meta layer: the mod never explains itself to the player, but keeps its own records.")
                .push("meta");

        ENABLE_META_EVENTS = builder
                .comment("Включить мета-слой: запись событий режиссёра в logs/thedirector_meta.log")
                .define("enableMetaEvents", false);

        ENABLE_META_SCREENSHOTS = builder
                .comment("Делать скриншоты в момент крупных событий (клиентская часть мета-слоя)")
                .define("enableMetaScreenshots", false);

        LOG_EVENTS = builder
                .comment("Писать каждое событие режиссёра в обычный лог сервера")
                .define("logEvents", false);

        builder.pop();

        builder.comment("Измерение сна / Dream dimension").push("dream");

        ENABLE_DREAM_DIMENSION = builder
                .comment("Разрешить попадание в измерение 'thedirector:dream' через сон")
                .define("enableDreamDimension", true);

        DREAM_CHANCE = builder
                .comment("Базовый шанс попасть в сон при пробуждении (0.0 - 1.0).",
                         "Реальный шанс = dreamChance + dreadLevel / 200.0, то есть страх повышает вероятность.")
                .defineInRange("dreamChance", 0.3D, 0.0D, 1.0D);

        DREAM_DURATION_MINUTES = builder
                .comment("Сколько игровых минут игрок может находиться во сне до принудительного возврата")
                .defineInRange("dreamDurationMinutes", 10, 1, 240);

        builder.pop();

        builder.comment("Режиссура / Direction").push("director");

        EVENT_FREQUENCY = builder
                .comment("Частота опроса режиссёра в тиках (по умолчанию 100 тиков = 5 секунд)")
                .defineInRange("eventFrequency", 100, 20, 6000);

        ENABLE_TORCH_FLICKER = builder
                .comment("Мерцание факелов рядом с игроком (тушение и возврат огня)")
                .define("enableTorchFlicker", true);

        ACT_DAY_SCALE = builder
                .comment("Множитель длительности актов. 1 = как задумано (5/10/10/бесконечно дней),",
                         "2 = акты идут вдвое быстрее. Удобно для отладки.")
                .defineInRange("actDayScale", 1, 1, 20);

        builder.pop();

        SPEC = builder.build();
    }

    private Config() {
    }

    /** Мета-слой включён? */
    public static boolean metaEvents() {
        return ENABLE_META_EVENTS.get();
    }

    /** Скриншоты мета-слоя включены? */
    public static boolean metaScreenshots() {
        return ENABLE_META_SCREENSHOTS.get();
    }

    /** Логировать события в общий лог? */
    public static boolean logEvents() {
        return LOG_EVENTS.get();
    }

    /** Измерение сна включено? */
    public static boolean dreamEnabled() {
        return ENABLE_DREAM_DIMENSION.get();
    }

    /** Базовый шанс сна. */
    public static double dreamChance() {
        return DREAM_CHANCE.get();
    }

    /** Частота опроса режиссёра в тиках. */
    public static int eventFrequency() {
        return EVENT_FREQUENCY.get();
    }

    /** Лимит нахождения во сне в тиках. */
    public static int dreamDurationTicks() {
        return DREAM_DURATION_MINUTES.get() * 20 * 60;
    }

    /** Мерцание факелов включено? */
    public static boolean torchFlicker() {
        return ENABLE_TORCH_FLICKER.get();
    }

    /** Множитель длительности актов. */
    public static int actDayScale() {
        return ACT_DAY_SCALE.get();
    }
}
