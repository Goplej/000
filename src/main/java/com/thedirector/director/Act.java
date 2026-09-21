package com.thedirector.director;

import com.thedirector.Config;

/**
 * Четыре акта драматургической дуги.
 *
 * <p>Акт определяется количеством дней в мире и никогда не откатывается назад:
 * игрок не может "отыграть" испуг.</p>
 */
public enum Act {

    /** Дни 1-5. Смутное беспокойство: только атмосфера и микро-странности. */
    ONE(1, 1, 5, 0),
    /** Дни 5-15. Понимание, что что-то не так: следы, шёпот, сообщения. */
    TWO(2, 5, 15, 0),
    /** Дни 15-25. Вторжение: сущность входит в личное пространство. */
    THREE(3, 15, 25, 0),
    /** Дни 25+. Точка невозврата: мир не безопасен нигде. */
    FOUR(4, 25, Integer.MAX_VALUE, 0);

    private final int number;
    private final int fromDay;
    private final int toDay;
    private final int reserved;

    Act(int number, int fromDay, int toDay, int reserved) {
        this.number = number;
        this.fromDay = fromDay;
        this.toDay = toDay;
        this.reserved = reserved;
    }

    public int number() {
        return number;
    }

    public int fromDay() {
        return fromDay;
    }

    public int toDay() {
        return toDay;
    }

    /** Длительность акта в игровых днях с учётом множителя конфига. */
    public int scaledLength() {
        int scale = Math.max(1, Config.actDayScale());
        return (int) Math.ceil((toDay - fromDay) / (double) scale);
    }

    /** Сколько дней мира нужно, чтобы акт начался (с учётом множителя конфига). */
    public int scaledStartDay() {
        if (this == ONE) {
            return 1;
        }
        int scale = Math.max(1, Config.actDayScale());
        return (int) Math.ceil((fromDay - 1) / (double) scale) + 1;
    }

    /** Человеческое название акта (для логов режиссёра). */
    public String label() {
        return switch (this) {
            case ONE -> "Акт 1: смутное беспокойство";
            case TWO -> "Акт 2: что-то не так";
            case THREE -> "Акт 3: вторжение";
            case FOUR -> "Акт 4: точка невозврата";
        };
    }

    /** Определить акт по количеству дней в мире. */
    public static Act ofDay(long days) {
        Act result = ONE;
        for (Act act : values()) {
            if (days >= act.scaledStartDay()) {
                result = act;
            }
        }
        return result;
    }

    public static Act of(int number) {
        for (Act act : values()) {
            if (act.number == number) {
                return act;
            }
        }
        return ONE;
    }

    public int reserved() {
        return reserved;
    }
}
