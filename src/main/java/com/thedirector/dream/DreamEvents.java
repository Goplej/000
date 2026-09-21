package com.thedirector.dream;

import com.thedirector.Config;
import com.thedirector.ModSounds;
import com.thedirector.TheDirector;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.MetaLayer;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageTypes;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraftforge.event.entity.EntityJoinLevelEvent;
import net.minecraftforge.event.entity.living.LivingDeathEvent;
import net.minecraftforge.event.entity.living.LivingHurtEvent;
import net.minecraftforge.event.entity.player.PlayerEvent;
import net.minecraftforge.event.entity.player.PlayerSleepInBedEvent;
import net.minecraftforge.event.entity.player.PlayerWakeUpEvent;
import net.minecraftforge.event.level.BlockEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;

/**
 * Правила сна.
 *
 * <p>Здесь собрано всё, что отличает сон от обычного мира: вход через сон, отсутствие
 * мобов, отсутствие текущей воды, неуязвимость к lava/void, возвращение блоков,
 * невозможность поспать внутри сна и "смерть как выход".</p>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class DreamEvents {

    private DreamEvents() {
    }

    // -------------------------------------------------------------- вход в сон

    /** Пробуждение после сна: единственная дверь в измерение сна. */
    @SubscribeEvent
    public static void onWakeUp(PlayerWakeUpEvent event) {
        if (!(event.getEntity() instanceof ServerPlayer player)) {
            return;
        }
        // Если игрока разбудили (урон, взрыв) — сон не случается
        if (event.wakeImmediately() || !event.updateLevel()) {
            return;
        }
        if (DreamDimension.isDream(player.level())) {
            return;
        }
        PlayerMemory memory = PlayerMemory.of(player);
        double roll = player.getRandom().nextDouble();
        if (!DreamDimension.tryEnter(player, memory, roll)) {
            return;
        }
        // Через два тика мир вокруг станет копией, а игрок окажется внутри неё
        ScheduledTasks.schedule(player.level(), 2, () -> {
            if (!DreamDimension.isDream(player.level())) {
                player.sendSystemMessage(net.minecraft.network.chat.Component.literal("\u00A78> \u00A77..."), true);
            }
        });
    }

    /** Внутри сна кровать не работает. */
    @SubscribeEvent
    public static void onSleepInBed(PlayerSleepInBedEvent event) {
        if (DreamDimension.isDream(event.getEntity().level())) {
            event.setResult(Player.BedSleepingProblem.OTHER_PROBLEM);
            return;
        }
        // Дом игрока определяется по ночёвкам: три ночи в одном месте — это база
        if (event.getEntity() instanceof ServerPlayer player && event.getOptionalPos().isPresent()) {
            PlayerMemory memory = PlayerMemory.of(player);
            BlockPos bed = event.getOptionalPos().get();
            if (!memory.hasBase()) {
                memory.setBaseVisits(memory.getBaseVisits() + 1);
                if (memory.getBaseVisits() >= 3) {
                    memory.setBaseLocation(bed);
                    TheDirector.LOGGER.info("[The Director] Дом игрока определён: {}", bed);
                }
            } else if (memory.getBaseLocation().distSqr(bed) > 64 * 64) {
                // Игрок сменил дом — режиссёр запоминает новое место
                memory.setBaseVisits(memory.getBaseVisits() + 1);
                if (memory.getBaseVisits() >= 3) {
                    memory.setBaseLocation(bed);
                }
            }
        }
    }

    // -------------------------------------------------------------- правила сна

    /** Во сне нет мобов: мир пуст. */
    @SubscribeEvent
    public static void onEntityJoin(EntityJoinLevelEvent event) {
        if (!DreamDimension.isDream(event.getLevel())) {
            return;
        }
        if (event.getEntity() instanceof Mob) {
            event.setCanceled(true);
        }
    }

    /** Разбитый блок возвращается через секунду. */
    @SubscribeEvent
    public static void onBlockBreak(BlockEvent.BreakEvent event) {
        if (!(event.getLevel() instanceof ServerLevel level) || !DreamDimension.isDream(level)) {
            return;
        }
        BlockPos pos = event.getPos().immutable();
        BlockState original = level.getBlockState(pos);
        ScheduledTasks.schedule(level, 20, () -> level.setBlock(pos, original, 3));
    }

    /** Поставленный во сне блок тоже запоминается, чтобы сон можно было убрать целиком. */
    @SubscribeEvent
    public static void onBlockPlace(BlockEvent.EntityPlaceEvent event) {
        if (!(event.getLevel() instanceof ServerLevel level) || !DreamDimension.isDream(level)) {
            return;
        }
        DreamReplicator.mark(event.getPos().immutable(), level.getBlockState(event.getPos()));
    }

    /** Вода не течёт, лава не растекается. */
    @SubscribeEvent
    public static void onFluidPlace(BlockEvent.FluidPlaceBlockEvent event) {
        if (event.getLevel() instanceof ServerLevel level && DreamDimension.isDream(level)) {
            event.setCanceled(true);
        }
    }

    /** Во сне нельзя угореть, утонуть, сгореть и упасть в пустоту. */
    @SubscribeEvent
    public static void onHurt(LivingHurtEvent event) {
        if (!(event.getEntity() instanceof ServerPlayer player) || !DreamDimension.isDream(player.level())) {
            return;
        }
        var source = event.getSource();
        if (source.is(DamageTypes.LAVA) || source.is(DamageTypes.HOT_FLOOR)
                || source.is(DamageTypes.IN_FIRE) || source.is(DamageTypes.ON_FIRE)
                || source.is(DamageTypes.DROWN) || source.is(DamageTypes.FELL_OUT_OF_WORLD)
                || source.is(DamageTypes.STARVE) || source.is(DamageTypes.FALL)) {
            event.setCanceled(true);
        }
    }

    /**
     * Смерть во сне — это выход, а не смерть.
     * Игрок просыпается там, где лёг, потеряв одну вещь (она уходит в память режиссёра).
     */
    @SubscribeEvent
    public static void onDeath(LivingDeathEvent event) {
        if (!(event.getEntity() instanceof ServerPlayer player) || !DreamDimension.isDream(player.level())) {
            return;
        }
        event.setCanceled(true);
        PlayerMemory memory = PlayerMemory.of(player);

        // Потеря: одна случайная вещь из инвентаря не возвращается
        for (int slot = 0; slot < 36; slot++) {
            var stack = player.getInventory().getItem(slot);
            if (!stack.isEmpty()) {
                memory.rememberLostItem(stack.copy());
                player.getInventory().setItem(slot, net.minecraft.world.item.ItemStack.EMPTY);
                break;
            }
        }
        player.setHealth(Math.max(6.0F, player.getHealth()));
        DreamDimension.leave(player, memory);
        MetaLayer.note(player, "DREAM_DEATH_EXIT (потеря предмета)");
    }

    /** Если игрок всё же падает в пустоту сна — он возвращается к точке входа. */
    @SubscribeEvent
    public static void onPlayerTick(net.minecraftforge.event.TickEvent.PlayerTickEvent event) {
        if (event.phase != net.minecraftforge.event.TickEvent.Phase.END) {
            return;
        }
        if (!(event.player instanceof ServerPlayer player) || !DreamDimension.isDream(player.level())) {
            return;
        }
        ServerLevel level = player.serverLevel();
        if (player.getY() < level.getMinBuildHeight() + 2) {
            BlockPos anchor = PlayerMemory.of(player).getDreamAnchor();
            if (anchor != null) {
                player.teleportTo(level, anchor.getX() + 0.5D, anchor.getY() + 1.0D, anchor.getZ() + 0.5D,
                        player.getYRot(), player.getXRot());
            } else {
                player.teleportTo(level, player.getX(), 90.0D, player.getZ(), player.getYRot(), player.getXRot());
            }
            level.playSound(null, player.getX(), player.getY(), player.getZ(),
                    ModSounds.VOID_AMBIENT.get(), net.minecraft.sounds.SoundSource.AMBIENT, 0.6F, 0.5F);
        }
        // Страховка: если игрок оказался во сне без якоря (например, через /execute)
        PlayerMemory memory = PlayerMemory.of(player);
        if (memory.getDreamAnchor() == null) {
            memory.setDreamAnchor(player.blockPosition());
        }
    }

    /** Игрок вышел и зашёл снова, находясь во сне — вернуть его в обычный мир. */
    @SubscribeEvent
    public static void onLoggedIn(PlayerEvent.PlayerLoggedInEvent event) {
        if (!(event.getEntity() instanceof ServerPlayer player)) {
            return;
        }
        if (!DreamDimension.isDream(player.level())) {
            return;
        }
        PlayerMemory memory = PlayerMemory.of(player);
        DreamDimension.leave(player, memory);
        TheDirector.LOGGER.info("[The Director] {} вышел из сна через перезаход.", player.getGameProfile().getName());
    }

    /** Проверка, включено ли измерение конфигом (используется датапаком/командами). */
    public static boolean enabled() {
        return Config.dreamEnabled();
    }

    /** Служебный доступ к измерению сна (для отладки). */
    public static Level levelOf(ServerPlayer player) {
        return player.server.getLevel(DreamDimension.DREAM_KEY);
    }
}
