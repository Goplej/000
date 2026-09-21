package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CChatLinePacket;
import com.thedirector.network.packet.S2CFOVPacket;
import com.thedirector.network.packet.S2CShakeCameraPacket;
import com.thedirector.network.packet.S2CSoundAtPlayerPacket;
import com.thedirector.network.packet.S2CVignettePacket;
import com.thedirector.util.ScheduledTasks;
import com.thedirector.util.Texts;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

import java.util.List;

/**
 * "Наблюдатель" — главное событие акта 4.
 *
 * <p>Композиция из всех приёмов сразу: шёпот за спиной, шаги над головой, отключение света,
 * сужение зрения, тряска, сообщение "от самого игрока". Это не удар, а присутствие:
 * после него остаётся уверенность, что кто-то стоял рядом и смотрел.</p>
 */
public class WatcherEvent extends DirectorEvent {

    public WatcherEvent() {
        super("watcher", Act.FOUR, 22, 4, 8, 20 * 60 * 12);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }

        // 1. Шёпот за спиной
        Vec3 behind = behind(player, 2.0D);
        playSound(level, behind.x, behind.y, behind.z, ModSounds.WHISPER.get(), 0.8F, 0.7F);

        // 2. Сообщение "от игрока"
        NetworkHandler.toPlayer(new S2CChatLinePacket(
                net.minecraft.network.chat.Component.literal("<" + serverPlayer.getGameProfile().getName() + "> ")
                        .append(net.minecraft.network.chat.Component.translatable("thedirector.msg.behind"))), serverPlayer);

        // 3. Свет уходит
        List<BlockPos> lights = Search.findLights(level, player.blockPosition(), 20, 8);
        for (BlockPos pos : lights) {
            BlockState original = level.getBlockState(pos);
            level.setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
            ScheduledTasks.schedule(level, 140 + level.random.nextInt(100), () -> {
                if (level.getBlockState(pos).isAir()) {
                    level.setBlock(pos, original, 3);
                }
            });
        }

        // 4. Тело реагирует раньше головы
        NetworkHandler.toPlayer(new S2CFOVPacket(0.6F, 200), serverPlayer);
        NetworkHandler.toPlayer(new S2CShakeCameraPacket(1.3F, 80), serverPlayer);
        NetworkHandler.toPlayer(new S2CVignettePacket(0.9F, 12.0F, 20 * 14), serverPlayer);
        NetworkHandler.toPlayer(new S2CSoundAtPlayerPacket(ModSounds.BREATH.getId(), 0.8F, 0.6F), serverPlayer);

        // 5. Вторая волна: он всё ещё здесь
        ScheduledTasks.schedule(level, 120, () -> {
            Vec3 closer = behind(player, 0.8D);
            playSound(level, closer.x, closer.y, closer.z, ModSounds.WHISPER.get(), 0.6F, 0.5F);
            NetworkHandler.toPlayer(new S2CShakeCameraPacket(0.7F, 40), serverPlayer);
        });
    }
}
