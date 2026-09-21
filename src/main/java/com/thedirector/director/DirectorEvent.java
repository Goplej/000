package com.thedirector.director;

import com.thedirector.memory.PlayerMemory;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

/**
 * Одно режиссёрское событие.
 *
 * <p>Каждое событие — отдельный класс с методом {@link #execute(Player, Level, PlayerMemory)}.
 * Событие обязано быть тихим снаружи: оно не должно оставлять в мире доказательств того,
 * что это мод (никаких невидимых сущностей, никаких подсказок в чате).</p>
 *
 * <p>Веса: чем выше {@code weight}, тем чаще событие выбирается, если оно доступно.
 * {@code dreadCost} — сколько страха получает игрок, {@code safetyCost} — сколько
 * ощущения безопасности у него забирают.</p>
 */
public abstract class DirectorEvent {

    private final String id;
    private final Act minAct;
    private final int weight;
    private final int dreadCost;
    private final int safetyCost;
    private final int cooldownTicks;

    protected DirectorEvent(String id, Act minAct, int weight, int dreadCost, int safetyCost, int cooldownTicks) {
        this.id = id;
        this.minAct = minAct;
        this.weight = weight;
        this.dreadCost = dreadCost;
        this.safetyCost = safetyCost;
        this.cooldownTicks = cooldownTicks;
    }

    /** Уникальный идентификатор события (пишется в историю игрока). */
    public final String id() {
        return id;
    }

    /** Минимальный акт, с которого событие доступно. */
    public final Act minAct() {
        return minAct;
    }

    /** Базовый вес при выборе. */
    public final int weight() {
        return weight;
    }

    /** Сколько страха добавляет событие. */
    public final int dreadCost() {
        return dreadCost;
    }

    /** Сколько безопасности снимает событие. */
    public final int safetyCost() {
        return safetyCost;
    }

    /** Персональный кулдаун события в тиках. */
    public final int cooldownTicks() {
        return cooldownTicks;
    }

    /** Может ли событие выполниться прямо сейчас (есть ли подходящий контекст). */
    public boolean canRun(ServerPlayer player, PlayerMemory memory, ServerLevel level) {
        return true;
    }

    /** Выполнить событие. */
    public abstract void execute(Player player, Level level, PlayerMemory memory);

    // ------------------------------------------------------------ утилиты

    /** Проиграть звук в мире от имени окружения (не от игрока, иначе он "видит" источник). */
    protected final void playSound(Level level, double x, double y, double z, SoundEvent sound, float volume, float pitch) {
        level.playSound(null, x, y, z, sound, SoundSource.AMBIENT, volume, pitch);
    }

    /** Позиция позади игрока на расстоянии {@code distance} (там, где он не смотрит). */
    protected final Vec3 behind(Player player, double distance) {
        double yaw = Math.toRadians(player.getYRot());
        return new Vec3(player.getX() + Math.sin(yaw) * distance,
                player.getY(),
                player.getZ() - Math.cos(yaw) * distance);
    }

    /** Позиция над головой игрока. */
    protected final Vec3 above(Player player, double height) {
        return new Vec3(player.getX(), player.getY() + height, player.getZ());
    }

    /** Поставить блок, если там воздух; вернуть позицию или null. */
    protected final net.minecraft.core.BlockPos placeIfAir(Level level, net.minecraft.core.BlockPos pos, BlockState state) {
        if (level.isEmptyBlock(pos) || level.getBlockState(pos).canBeReplaced()) {
            level.setBlock(pos, state, 3);
            return pos;
        }
        return null;
    }

    /** Отправить игроку сообщение. */
    protected final void tell(ServerPlayer player, Component component) {
        player.sendSystemMessage(component);
    }

    /** Ключ измерения (для планировщика задач). */
    protected final ResourceKey<Level> dimensionOf(Level level) {
        return level.dimension();
    }
}
