package com.thedirector.corruption;

import com.thedirector.ModSounds;
import com.thedirector.TheDirector;
import com.thedirector.dream.DreamDimension;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.MetaLayer;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraftforge.event.entity.player.PlayerInteractEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;

/**
 * Поведение испорченных предметов.
 *
 * <p>Два случая:</p>
 * <ul>
 *   <li>предмет-ключ ({@code dream_key}) — использовать его во сне означает проснуться.
 *       Это единственный "честный" способ выйти, и он требует догадки;</li>
 *   <li>предмет "наоборот" ({@code inverted}) — вместо пользы вредит: игрок ест золотое
 *       яблоко, а получает отравление.</li>
 * </ul>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class CorruptionEvents {

    private CorruptionEvents() {
    }

    /** Использование предмета в воздухе. */
    @SubscribeEvent
    public static void onRightClickItem(PlayerInteractEvent.RightClickItem event) {
        handle(event, event.getEntity(), event.getItemStack());
    }

    /** Использование предмета по блоку. */
    @SubscribeEvent
    public static void onRightClickBlock(PlayerInteractEvent.RightClickBlock event) {
        handle(event, event.getEntity(), event.getItemStack());
    }

    private static void handle(PlayerInteractEvent event, Player player, ItemStack stack) {
        if (player.level().isClientSide() || stack.isEmpty()) {
            return;
        }
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }

        // --- Ключ от сна ---
        if (ItemCorruptionSystem.isDreamKey(stack) && DreamDimension.isDream(player.level())) {
            PlayerMemory memory = PlayerMemory.of(serverPlayer);
            if (memory.getDreamVisits() > 0) {
                player.level().playSound(null, player.getX(), player.getY(), player.getZ(),
                        ModSounds.REVERSE.get(), SoundSource.AMBIENT, 0.9F, 0.6F);
                serverPlayer.sendSystemMessage(Component.literal("\u00A78> \u00A77..."));
                DreamDimension.leave(serverPlayer, memory);
                MetaLayer.note(serverPlayer, "DREAM_EXIT_BY_KEY item=" + stack.getItem());
                event.setCanceled(true);
                event.setCancellationResult(InteractionResult.SUCCESS);
                return;
            }
        }

        // --- Предмет работает наоборот ---
        if (ItemCorruptionSystem.isInverted(stack)) {
            player.level().playSound(null, player.getX(), player.getY(), player.getZ(),
                    ModSounds.REVERSE.get(), SoundSource.AMBIENT, 0.7F, 0.5F);
            serverPlayer.addEffect(new MobEffectInstance(MobEffects.CONFUSION, 20 * 12, 0));
            serverPlayer.addEffect(new MobEffectInstance(MobEffects.HUNGER, 20 * 8, 1));
            serverPlayer.hurt(serverPlayer.damageSources().magic(), 1.0F);
            event.setCanceled(true);
            event.setCancellationResult(InteractionResult.SUCCESS);
        }
    }
}
