package com.thedirector.util;

/**
 * Ключи NBT, которые использует система порчи предметов.
 */
public final class CorruptionTags {

    /** Имя "украденной" вещи (показывается вместо имени двойника). */
    public static final String STOLEN_NAME = "thedirector:stolen_name";
    /** Подпись: предмет испорчен. */
    public static final String CORRUPTED = "thedirector:corrupted";
    /** Подпись: предмет дублирован. */
    public static final String DUPLICATE = "thedirector:duplicate";
    /** Подпись: предмет-ключ, через который игрок выходит из сна. */
    public static final String DREAM_KEY = "thedirector:dream_key";
    /** Подпись: предмет "работает наоборот". */
    public static final String INVERTED = "thedirector:inverted";

    private CorruptionTags() {
    }
}
