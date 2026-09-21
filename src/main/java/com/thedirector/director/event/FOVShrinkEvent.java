package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CFOVPacket;
import com.thedirector.network.packet.S2CShakeCameraPacket;
import com.thedirector.network.packet.S2CVignettePacket;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Сужение FOV.
 *
 * <p>Игрок видит меньшую часть мира и не может понять, почему: настройки он не менял.
 * Классическая "поломка доверия к собственному восприятию".</p>
 */
public class FOVShrinkEvent extends DirectorEvent {

    public FOVShrinkEvent() {
        super("fov_shrink", Act.THREE, 14, 2, 3, 20 * 60 * 5);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }
        Act act = Act.of(memory.getCurrentAct());
        float multiplier = act == Act.FOUR ? 0.62F : 0.72F;

        NetworkHandler.toPlayer(new S2CFOVPacket(multiplier, 20 * 8), serverPlayer);
        NetworkHandler.toPlayer(new S2CShakeCameraPacket(0.5F, 40), serverPlayer);
        NetworkHandler.toPlayer(new S2CVignettePacket(0.45F, 28.0F, 20 * 9), serverPlayer);
    }
}
