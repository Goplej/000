package com.thedirector.director.event;

import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CVignettePacket;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Гроза при ясном небе.
 *
 * <p>Акт 4: "даже днём, даже в деревне". Погода меняется без причины и без предупреждения,
 * а следом возвращается солнце. Игрок не может отсидеться: безопасного времени суток нет.</p>
 */
public class DaylightThunderEvent extends DirectorEvent {

    public DaylightThunderEvent() {
        super("daylight_thunder", Act.FOUR, 18, 3, 6, 20 * 60 * 4);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, ServerLevel level) {
        return level.dimension().equals(Level.OVERWORLD) && player.getY() > 60.0D;
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer) || !(level instanceof ServerLevel serverLevel)) {
            return;
        }
        // Гроза на 20-40 секунд, затем небо снова чистое
        int duration = 400 + level.random.nextInt(400);
        serverLevel.setWeatherParameters(0, duration, true, true);

        playSound(level, player.getX(), player.getY() + 20.0D, player.getZ(), ModSounds.THUNDER.get(), 1.0F, 0.8F);
        NetworkHandler.toPlayer(new S2CVignettePacket(0.3F, 40.0F, duration), serverPlayer);
    }
}
