package com.thedirector.util;

import com.thedirector.director.Act;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.MutableComponent;
import net.minecraft.network.chat.Style;
import net.minecraft.util.RandomSource;

/**
 * Тексты режиссёра.
 *
 * <p>Правило мода: режиссёр никогда ничего не объясняет. Ни одного сообщения о том,
 * что произошло, — только обрывки, которые игрок достраивает сам.</p>
 */
public final class Texts {

    /** Обрывки сообщений по актам (ключи локализации). */
    private static final String[][] MESSAGES = {
            {},
            {
                    "thedirector.msg.act1"
            },
            {
                    "thedirector.msg.watch",
                    "thedirector.msg.behind",
                    "thedirector.msg.remember",
                    "thedirector.msg.act1"
            },
            {
                    "thedirector.msg.return",
                    "thedirector.msg.dream",
                    "thedirector.msg.item",
                    "thedirector.msg.behind"
            }
    };

    /** Стили: чем выше акт, тем "тише" выглядит текст. */
    private static final Style[] STYLES = {
            Style.EMPTY,
            Style.EMPTY.withColor(0x777777),
            Style.EMPTY.withColor(0x555555).withItalic(true),
            Style.EMPTY.withColor(0x3A0A0A).withItalic(true)
    };

    /** Случайный обрывок для текущего акта. */
    public static MutableComponent randomMessage(Act act, RandomSource random) {
        String[] pool = MESSAGES[Math.max(0, Math.min(MESSAGES.length - 1, act.number()))];
        if (pool.length == 0) {
            return Component.literal("").withStyle(STYLES[0]);
        }
        return Component.translatable(pool[random.nextInt(pool.length)])
                .withStyle(STYLES[Math.min(STYLES.length - 1, act.number())]);
    }

    /**
     * "Сбой" — сообщение, выглядящее как ошибка сервера.
     * Используется в акте 4, когда режиссёр перестаёт притворяться.
     */
    public static MutableComponent glitchMessage(RandomSource random) {
        String[] pool = {
                "§8> §7connection lost",
                "§8> §7chunk read error  §8[" + random.nextInt(9999) + "]",
                "§8> §7world seed accepted",
                "§8> §7you",
                "§8> §7§ohe is awake",
                "§8> §7entity 0x" + Integer.toHexString(random.nextInt(0xFFFF)).toUpperCase()
        };
        return Component.literal(pool[random.nextInt(pool.length)]);
    }

    /** Имена, которые получают испорченные предметы. */
    private static final String[] CORRUPTED_NAMES = {
            "\u0415\u0433\u043e \u0432\u0435\u0449\u044c",
            "\u041d\u0435 \u0442\u0432\u043e\u0451",
            "\u0422\u0430\u043a\u043e\u0435 \u0436\u0435",
            "\u0422\u0432\u043e\u0451?",
            "\u041e\u043d \u0434\u0435\u0440\u0436\u0430\u043b \u044d\u0442\u043e",
            "\u041f\u043e\u043c\u043d\u0438\u0448\u044c?",
            "\u041e\u043d\u043e \u0435\u0449\u0451 \u0442\u0451\u043f\u043b\u043e\u0435",
            "."
    };

    /** Случайное "жуткое" имя для испорченного предмета. */
    public static String corruptedItemName(RandomSource random) {
        return CORRUPTED_NAMES[random.nextInt(CORRUPTED_NAMES.length)];
    }

    /**
     * Заголовок окна, который подменяет режиссёр.
     * Использует символы нулевой ширины, поэтому название выглядит "пустым" и неверным.
     */
    public static String corruptedWindowTitle() {
        String[] pool = {
                "Minecraft* 1.20.1\u200B",
                "\u2060\u2060\u2060\u2060",
                "Minecraft\u200B 1.20.1 - \u2063",
                "Minecraft* \u2060\u2060\u2060\u2060"
        };
        return pool[System.nanoTime() % pool.length < 0 ? 0 : (int) (Math.abs(System.nanoTime()) % pool.length)];
    }

    private Texts() {
    }
}
