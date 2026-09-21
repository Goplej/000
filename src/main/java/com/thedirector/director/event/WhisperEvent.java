package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.ScheduledTasks;
import com.thedirector.util.Texts;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;

/**
 * Шёпот за спиной.
 *
 * <p>Звук приходит ровно из той точки, куда игрок не смотрит. В акте 3 и выше
 * через пару секунд приходит "дыхание" — доказательство, что источник не исчез,
 * а просто перестал говорить.</p>
 */
public class WhisperEvent extends DirectorEvent {

    public WhisperEvent() {
        super("whisper", Act.ONE, 20, 1, 2, 20 * 60 * 3);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        Act act = Act.of(memory.getCurrentAct());
        Vec3 origin = behind(player, 3.0D + level.random.nextDouble() * 4.0D);

        playSound(level, origin.x, origin.y, origin.z, ModSounds.WHISPER.get(),
                0.6F + act.number() * 0.1F, 0.75F + level.random.nextFloat() * 0.2F);

        if (act.number() >= 2 && level.random.nextFloat() < 0.35F && player instanceof ServerPlayer serverPlayer) {
            tell(serverPlayer, Texts.randomMessage(act, level.random));
        }

        if (act.number() >= 3) {
            ScheduledTasks.schedule(level, 40, () -> playSound(level, origin.x, origin.y, origin.z,
                    ModSounds.BREATH.get(), 0.5F, 0.7F));
        }

        // В акте 4 шёпот иногда доносится из-за спины ещё раз — уже ближе
        if (act == Act.FOUR) {
            Vec3 closer = behind(player, 1.5D);
            ScheduledTasks.schedule(level, 70, () -> playSound(level, closer.x, closer.y, closer.z,
                    ModSounds.WHISPER.get(), 0.45F, 0.6F));
        }
    }
}
