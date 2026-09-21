package com.thedirector.memory;

import com.thedirector.Config;
import com.thedirector.TheDirector;
import com.thedirector.director.Director;
import com.thedirector.director.event.support.Search;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraftforge.event.AttachCapabilitiesEvent;
import net.minecraftforge.event.TickEvent;
import net.minecraftforge.event.entity.item.ItemTossEvent;
import net.minecraftforge.event.entity.player.PlayerEvent;
import net.minecraftforge.event.entity.player.PlayerInteractEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;

/**
 * Жизненный цикл памяти игрока и точка входа режиссёра.
 *
 * <p>Память вешается как capability, копируется при смерти и обновляется каждый тик.
 * Отсюда же раз в {@code eventFrequency} тиков вызывается {@link Director}.</p>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class PlayerMemoryEvents {

    private static final ResourceLocation MEMORY_ID = new ResourceLocation(TheDirector.MOD_ID, "memory");

    private PlayerMemoryEvents() {
    }

    /** Память есть у любого игрока: и в одиночной игре, и на сервере. */
    @SubscribeEvent
    public static void onAttachCapabilities(AttachCapabilitiesEvent<Entity> event) {
        if (event.getObject() instanceof Player) {
            event.addCapability(MEMORY_ID, new PlayerMemoryProvider());
        }
    }

    /** Смерть и возврат к точке возрождения не должны стирать память. */
    @SubscribeEvent
    public static void onClone(PlayerEvent.Clone event) {
        event.getOriginal().reviveCaps();
        try {
            event.getOriginal().getCapability(PlayerMemory.CAPABILITY).ifPresent(old ->
                    event.getEntity().getCapability(PlayerMemory.CAPABILITY).ifPresent(fresh -> fresh.copyFrom(old)));
        } finally {
            event.getOriginal().invalidateCaps();
        }
    }

    /** Первый вход: считаем дни мира, чтобы акт соответствовал реальности. */
    @SubscribeEvent
    public static void onLogin(PlayerEvent.PlayerLoggedInEvent event) {
        if (!(event.getEntity() instanceof ServerPlayer player)) {
            return;
        }
        PlayerMemory memory = PlayerMemory.of(player);
        long worldDay = player.serverLevel().getDayTime() / 24000L + 1L;
        if (memory.getDaysInWorld() < worldDay) {
            memory.setDaysInWorld(worldDay);
        }
    }

    /** Основной тик: обновление памяти и вызов режиссёра. */
    @SubscribeEvent
    public static void onPlayerTick(TickEvent.PlayerTickEvent event) {
        if (event.phase != TickEvent.Phase.END || !(event.player instanceof ServerPlayer player)) {
            return;
        }
        PlayerMemory memory = PlayerMemory.of(player);
        memory.tickPassive(player);
        Director.onPlayerTick(player, memory);
    }

    /** Игрок выбросил вещь — режиссёр это запомнил. */
    @SubscribeEvent
    public static void onItemToss(ItemTossEvent event) {
        if (!(event.getPlayer() instanceof ServerPlayer player)) {
            return;
        }
        PlayerMemory.of(player).rememberLostItem(event.getEntity().getItem().copy());
    }

    /** Учёт источников света: игрок сам показывает, чего он боится. */
    @SubscribeEvent
    public static void onPlaceLight(PlayerInteractEvent.RightClickBlock event) {
        if (event.getLevel().isClientSide() || !(event.getEntity() instanceof ServerPlayer player)) {
            return;
        }
        ItemStack stack = event.getItemStack();
        if (stack.isEmpty()) {
            return;
        }
        if (stack.is(net.minecraft.world.item.Items.TORCH)
                || stack.is(net.minecraft.world.item.Items.LANTERN)
                || stack.is(net.minecraft.world.item.Items.SOUL_TORCH)
                || stack.is(net.minecraft.world.item.Items.SOUL_LANTERN)
                || stack.is(net.minecraft.world.item.Items.CAMPFIRE)
                || stack.is(net.minecraft.world.item.Items.GLOWSTONE)
                || stack.is(net.minecraft.world.item.Items.SHROOMLIGHT)) {
            PlayerMemory.of(player).addLightUsage(1);
        }
    }

    /** Проверка "стоит ли игрок рядом со своим домом" — используется событиями. */
    public static boolean atBase(ServerPlayer player, PlayerMemory memory) {
        return memory.hasBase() && memory.getBaseLocation().distSqr(player.blockPosition()) < 64 * 64;
    }

    /** Текущее время молчания в тиках (для отладки и логов). */
    public static long silenceTicks(ServerPlayer player, PlayerMemory memory) {
        long now = player.serverLevel().getGameTime();
        return memory.getLastEventTick() == 0L ? -1L : now - memory.getLastEventTick();
    }

    /** Сколько света игрок поставил (для отчёта в мета-журнале). */
    public static int lightUsage(PlayerMemory memory) {
        return memory.getLightUsage();
    }

    /** Является ли предмет источником света, который мод умеет гасить. */
    public static boolean isLightSource(ItemStack stack) {
        if (!(stack.getItem() instanceof net.minecraft.world.item.BlockItem blockItem)) {
            return false;
        }
        return Search.isLightSource(blockItem.getBlock().defaultBlockState());
    }

    /** Включён ли мод вообще (на случай отключения режиссёра в конфиге). */
    public static boolean enabled() {
        return Config.eventFrequency() > 0;
    }
}
