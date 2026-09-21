package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Шаги на крыше дома игрока.
 *
 * <p>Требование к событию: игрок должен находиться внутри и слышать шаги над собой.
 * Шаги идут по направлению к игроку и заканчиваются "внутри" дома — то есть там,
 * где крыши уже нет. Это подрывает ощущение, что дом защищает.</p>
 */
public class StepOnRoofEvent extends DirectorEvent {

    public StepOnRoofEvent() {
        super("roof_steps", Act.THREE, 16, 3, 5, 20 * 60 * 6);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, ServerLevel level) {
        if (!memory.hasBase() || memory.getBaseLocation().distSqr(player.blockPosition()) > 48 * 48) {
            return false;
        }
        return Search.hasRoofAbove(level, player.blockPosition(), 8);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        BlockPos base = memory.getBaseLocation();
        if (base == null) {
            return;
        }
        Act act = Act.of(memory.getCurrentAct());
        int steps = 3 + level.random.nextInt(act == Act.FOUR ? 8 : 4);

        double startX = base.getX() + (level.random.nextDouble() - 0.5D) * 10.0D;
        double startZ = base.getZ() + (level.random.nextDouble() - 0.5D) * 10.0D;
        double stepX = (player.getX() - startX) / steps;
        double stepZ = (player.getZ() - startZ) / steps;
        double roofY = player.getY() + 3.0D + level.random.nextDouble() * 2.0D;

        for (int i = 0; i < steps; i++) {
            double x = startX + stepX * i;
            double z = startZ + stepZ * i;
            int delay = i * (12 + level.random.nextInt(6));
            ScheduledTasks.schedule(level, delay, () -> playSound(level, x, roofY, z,
                    ModSounds.ROOF_STEP.get(), 0.6F, 0.9F + level.random.nextFloat() * 0.15F));
        }

        // Последний шаг звучит уже ровно над головой игрока и чуть ниже — "в доме"
        int tail = steps * 14 + 10;
        ScheduledTasks.schedule(level, tail, () -> playSound(level, player.getX(), player.getY() + 2.2D,
                player.getZ(), ModSounds.ROOF_STEP.get(), 0.5F, 0.8F));
        ScheduledTasks.schedule(level, tail + 30, () -> playSound(level, player.getX(), player.getY() + 1.0D,
                player.getZ(), ModSounds.WOOD_CREAK.get(), 0.45F, 0.6F));
    }
}
