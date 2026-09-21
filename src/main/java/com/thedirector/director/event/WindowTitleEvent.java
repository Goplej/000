package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CWindowTitlePacket;
import com.thedirector.util.ScheduledTasks;
import com.thedirector.util.Texts;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Подмена заголовка окна.
 *
 * <p>Эффект рассчитан на отложенное восприятие: игрок свернёт игру, и через час
 * увидит в панели задач окно с пустым или искажённым названием. Событие специально
 * не оставляет следов внутри игры.</p>
 */
public class WindowTitleEvent extends DirectorEvent {

    public WindowTitleEvent() {
        super("window_title", Act.ONE, 12, 1, 1, 20 * 60 * 8);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }
        Act act = Act.of(memory.getCurrentAct());
        int hold = act.number() >= 3 ? 20 * 20 : 20 * 8;

        NetworkHandler.toPlayer(new S2CWindowTitlePacket(Texts.corruptedWindowTitle(), hold), serverPlayer);

        // Заголовок возвращается сам — игрок снова сомневается в себе
        ScheduledTasks.schedule(level, hold + 5, () -> NetworkHandler.toPlayer(new S2CWindowTitlePacket("", 0), serverPlayer));
    }
}
