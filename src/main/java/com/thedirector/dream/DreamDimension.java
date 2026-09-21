package com.thedirector.dream;

import com.thedirector.Config;
import com.thedirector.TheDirector;
import com.thedirector.director.Act;
import com.thedirector.corruption.ItemCorruptionSystem;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CVignettePacket;
import com.thedirector.util.MetaLayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.Level;

/**
 * Измерение сна.
 *
 * <p>Сон — это <b>копия</b> мира игрока, из которой убрали всё, что делает мир живым:
 * нет мобов, нет звуков кроме собственных шагов, солнце стоит на месте, время не идёт,
 * вода не течёт, огонь не жжёт, кровать не работает, разбитые блоки возвращаются.</p>
 *
 * <p>Единственный способ выйти самому — использовать свою вещь, которую вернул режиссёр
 * (предмет с меткой {@code thedirector:dream_key}). Через 10 игровых минут сон отпускает
 * игрока сам.</p>
 */
public final class DreamDimension {

    /** Ключ измерения; тип и генератор описаны датапаком в data/thedirector/dimension/. */
    public static final ResourceKey<Level> DREAM_KEY =
            ResourceKey.create(Registries.DIMENSION, new net.minecraft.resources.ResourceLocation(TheDirector.MOD_ID, "dream"));

    /** Время суток во сне: полдень, который никогда не заканчивается. */
    public static final long FROZEN_TIME = 6000L;

    private DreamDimension() {
    }

    /** Это измерение сна? Работает и на клиенте (по ключу измерения). */
    public static boolean isDream(Level level) {
        return level != null && level.dimension().equals(DREAM_KEY);
    }

    /** Получить измерение сна на сервере (или null, если датапак не загружен). */
    public static ServerLevel getDream(MinecraftServer server) {
        return server.getLevel(DREAM_KEY);
    }

    /** Шанс попасть в сон: базовый из конфига + страх игрока. */
    public static double dreamChance(PlayerMemory memory) {
        return Math.min(1.0D, Config.dreamChance() + memory.getDreadLevel() / 200.0D);
    }

    /**
     * Попытка входа в сон после пробуждения.
     *
     * @return {@code true}, если сон начался (или начал строиться)
     */
    public static boolean tryEnter(ServerPlayer player, PlayerMemory memory, double roll) {
        return tryEnter(player, memory, roll, false);
    }

    /**
     * Попытка входа в сон после пробуждения.
     *
     * @param force {@code true} — команда {@code /thedirector dream}: проверки шанса и
     *              правила «один сон в игровой день» пропускаются
     * @return {@code true}, если сон начался (или начал строиться)
     */
    public static boolean tryEnter(ServerPlayer player, PlayerMemory memory, double roll, boolean force) {
        if (!Config.dreamEnabled()) {
            return false;
        }
        // Один сон в день: режиссёр не повторяется
        long day = player.serverLevel().getDayTime() / 24000L;
        if (!force) {
            if (memory.getLastDreamAttemptDay() == day) {
                return false;
            }
            if (roll > dreamChance(memory)) {
                return false;
            }
        }

        MinecraftServer server = player.server;
        ServerLevel dream = getDream(server);
        ServerLevel overworld = server.getLevel(Level.OVERWORLD);
        if (dream == null || overworld == null) {
            // Датапак измерения не загружен — сон просто не приходит
            return false;
        }

        memory.setLastDreamAttemptDay(day);
        memory.setDreamVisits(memory.getDreamVisits() + 1);
        memory.setReturnAnchor(player.blockPosition());
        memory.clearDream();
        memory.setSleeping(true);
        memory.snapshotInventory(player);

        BlockPos center = memory.hasBase() ? memory.getBaseLocation() : player.blockPosition();
        ItemCorruptionSystem.prepareKey(player, memory);

        // Строим копию мира; игрок попадёт в неё, когда она будет готова
        DreamReplicator.begin(dream, overworld, center, player, () -> placePlayer(player, dream, memory));
        MetaLayer.note(player, "DREAM_ENTER visits=" + memory.getDreamVisits() + " dread=" + memory.getDreadLevel());
        return true;
    }

    /** Телепорт игрока в готовый сон. */
    private static void placePlayer(ServerPlayer player, ServerLevel dream, PlayerMemory memory) {
        BlockPos spot = DreamReplicator.spawnPoint();
        if (spot == null) {
            spot = player.blockPosition().atY(80);
        }
        spot = DreamReplicator.findSafeSpot(dream, spot);
        if (spot.getY() < dream.getMinBuildHeight() + 1) {
            spot = new BlockPos(spot.getX(), 80, spot.getZ());
        }
        dream.setDayTime(FROZEN_TIME);
        player.teleportTo(dream, spot.getX() + 0.5D, spot.getY(), spot.getZ() + 0.5D, player.getYRot(), player.getXRot());
        memory.clearDream();
        memory.setDreamAnchor(spot);

        // Порча предметов происходит только после входа: игрок уже не может отказаться
        ItemCorruptionSystem.corrupt(player, memory);

        NetworkHandler.toPlayer(new S2CVignettePacket(0.75F, 30.0F, 20 * 20), player);
        NetworkHandler.toPlayer(new com.thedirector.network.packet.S2CWindowTitlePacket(
                com.thedirector.util.Texts.corruptedWindowTitle(), 20 * 10), player);
        TheDirector.LOGGER.info("[The Director] {} во сне (посещение #{}).",
                player.getGameProfile().getName(), memory.getDreamVisits());
    }

    /** Принудительный выход из сна (по таймеру, по ключу или после "смерти"). */
    public static void leave(ServerPlayer player, PlayerMemory memory) {
        MinecraftServer server = player.server;
        ServerLevel overworld = server.getLevel(Level.OVERWORLD);
        // Возвращаем туда, где игрок лёг; если записи нет — в точку входа в сон
        BlockPos anchor = memory.getReturnAnchor();
        if (anchor == null) {
            anchor = memory.getDreamAnchor();
        }
        memory.clearDream();
        memory.setSleeping(false);
        memory.setReturnAnchor(null);

        if (overworld == null) {
            return;
        }
        if (anchor == null) {
            anchor = overworld.getSharedSpawnPos();
        }
        BlockPos safe = DreamReplicator.findSafeSpot(overworld, anchor);
        player.teleportTo(overworld, safe.getX() + 0.5D, safe.getY(), safe.getZ() + 0.5D,
                player.getYRot(), player.getXRot());
        MetaLayer.note(player, "DREAM_EXIT");
    }

    /** Поведение режиссёра, пока игрок внутри сна. */
    public static void directDream(ServerPlayer player, PlayerMemory memory, Act act) {
        long ticks = memory.getDreamTicks();

        // Принудительный возврат через 10 игровых минут
        if (ticks > Config.dreamDurationTicks()) {
            NetworkHandler.toPlayer(new S2CVignettePacket(0.9F, 8.0F, 60), player);
            leave(player, memory);
            return;
        }

        // Сон держит игрока в напряжении, но не спамит событиями
        if (ticks % 200L == 0L) {
            NetworkHandler.toPlayer(new S2CVignettePacket(0.5F + (float) Math.min(0.4D, ticks / 12000.0D), 34.0F, 240), player);
        }
        if (ticks == 20L * 60L) {
            // На первой минуте сна режиссёр показывает "подсказку" без слов
            NetworkHandler.toPlayer(new com.thedirector.network.packet.S2CShakeCameraPacket(0.4F, 30), player);
        }
        // Раз в минуту — отдалённый гул
        if (ticks % 1200L == 300L) {
            NetworkHandler.toPlayer(new com.thedirector.network.packet.S2CSoundAtPlayerPacket(
                    com.thedirector.ModSounds.VOID_AMBIENT.getId(), 0.5F, 0.5F), player);
        }
    }

    /** Может ли игрок сейчас спать в этом измерении. */
    public static boolean canSleepHere(Level level) {
        return !isDream(level);
    }
}
