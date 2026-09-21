package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

import java.util.List;

/**
 * Тушение света рядом с базой.
 *
 * <p>Свет — это мера контроля игрока над миром. Мод забирает его на несколько секунд и
 * возвращает: игрок остаётся с ощущением "мне показалось?". В акте 4 свет не возвращается.</p>
 */
public class TorchExtinguishEvent extends DirectorEvent {

    public TorchExtinguishEvent() {
        super("torch_extinguish", Act.TWO, 15, 2, 4, 20 * 60 * 5);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, net.minecraft.server.level.ServerLevel level) {
        BlockPos center = memory.hasBase() && memory.getBaseLocation().distSqr(player.blockPosition()) < 48 * 48
                ? memory.getBaseLocation()
                : player.blockPosition();
        return !Search.findLights(level, center, 16, 1).isEmpty();
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        Act act = Act.of(memory.getCurrentAct());
        BlockPos center = memory.hasBase() && memory.getBaseLocation().distSqr(player.blockPosition()) < 48 * 48
                ? memory.getBaseLocation()
                : player.blockPosition();

        List<BlockPos> lights = Search.findLights(level, center, 16, act == Act.FOUR ? 12 : 3 + level.random.nextInt(3));
        if (lights.isEmpty()) {
            return;
        }

        boolean temporary = act.number() <= 3;
        for (BlockPos pos : lights) {
            BlockState original = level.getBlockState(pos);
            level.setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
            playSound(level, pos.getX() + 0.5D, pos.getY() + 0.5D, pos.getZ() + 0.5D,
                    ModSounds.EXTINGUISH.get(), 0.8F, 0.9F + level.random.nextFloat() * 0.2F);

            if (temporary) {
                // Свет возвращается через 1-4 секунды, будто ничего не было
                int delay = 20 + level.random.nextInt(60);
                ScheduledTasks.schedule(level, delay, () -> {
                    if (level.getBlockState(pos).isAir()) {
                        level.setBlock(pos, original, 3);
                    }
                });
            }
        }
    }
}
