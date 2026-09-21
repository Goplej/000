package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CShakeCameraPacket;
import com.thedirector.network.packet.S2CVignettePacket;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

import java.util.List;

/**
 * Полное отключение света вокруг игрока.
 *
 * <p>Это "удар" в терминах концепции: за секунду до него была тишина, после — темнота.
 * Свет возвращается, но не весь и не сразу. В акте 4 часть источников не загорается
 * вообще — мир больше не обязан быть удобным.</p>
 */
public class LightsOutEvent extends DirectorEvent {

    public LightsOutEvent() {
        super("lights_out", Act.THREE, 14, 3, 6, 20 * 60 * 7);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, ServerLevel level) {
        return Search.findLights(level, player.blockPosition(), 24, 3).size() >= 3;
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        Act act = Act.of(memory.getCurrentAct());
        List<BlockPos> lights = Search.findLights(level, player.blockPosition(), 24, 10);
        if (lights.isEmpty()) {
            return;
        }

        for (BlockPos pos : lights) {
            BlockState original = level.getBlockState(pos);
            level.setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
            playSound(level, pos.getX() + 0.5D, pos.getY() + 0.5D, pos.getZ() + 0.5D,
                    ModSounds.EXTINGUISH.get(), 0.9F, 0.7F);

            boolean returns = act.number() <= 3 || level.random.nextFloat() < 0.6F;
            if (returns) {
                int delay = 100 + level.random.nextInt(80);
                ScheduledTasks.schedule(level, delay, () -> {
                    if (level.getBlockState(pos).isAir()) {
                        level.setBlock(pos, original, 3);
                    }
                });
            }
        }

        if (player instanceof ServerPlayer serverPlayer) {
            NetworkHandler.toPlayer(new S2CVignettePacket(0.85F, 14.0F, 20 * 12), serverPlayer);
            NetworkHandler.toPlayer(new S2CShakeCameraPacket(0.9F, 50), serverPlayer);
            ScheduledTasks.schedule(level, 90, () -> playSound(level, player.getX(), player.getY() + 1.5D,
                    player.getZ(), ModSounds.BREATH.get(), 0.7F, 0.6F));
        }
    }
}
