package com.thedirector.director.event;

import com.thedirector.Config;
import com.thedirector.ModSounds;
import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CCrashEndPacket;
import com.thedirector.network.packet.S2CFakeCrashPacket;
import com.thedirector.network.packet.S2CPlaySoundPacket;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

import java.util.List;

/**
 * Ложный краш.
 *
 * <p>Самый сильный приём мода. В стеке вызовов нет ни одного упоминания мода: только
 * ванильные классы. Игрок видит ошибку памяти, читает её, начинает искать решение —
 * и через семь секунд экран исчезает, а мир продолжается с того же места.</p>
 */
public class FakeCrashEvent extends DirectorEvent {

    /**
     * Правдоподобный стек ванильных классов.
     * Ни одна строка не указывает на мод — это принципиально.
     */
    private static final List<String> STACK_TRACE = List.of(
            "java.lang.OutOfMemoryError: Java heap space",
            "",
            "\tat net.minecraft.world.level.chunk.LevelChunk.<init>(LevelChunk.java:143)",
            "\tat net.minecraft.world.level.chunk.ChunkSource.getChunk(ChunkSource.java:97)",
            "\tat net.minecraft.client.renderer.LevelRenderer.renderChunkLayer(LevelRenderer.java:1187)",
            "\tat net.minecraft.client.renderer.LevelRenderer.renderLevel(LevelRenderer.java:1044)",
            "\tat net.minecraft.client.renderer.GameRenderer.renderLevel(GameRenderer.java:1177)",
            "\tat net.minecraft.client.renderer.GameRenderer.render(GameRenderer.java:988)",
            "\tat net.minecraft.client.Minecraft.runTick(Minecraft.java:1043)",
            "\tat net.minecraft.client.Minecraft.run(Minecraft.java:791)",
            "\tat java.base/java.lang.Thread.run(Thread.java:840)",
            "",
            "A detailed walkthrough of the error, its code path and all known details is as follows:",
            "--------------------------------------------------------------------------",
            "  Minecraft Version: 1.20.1",
            "  Operating System: linux (amd64) version 6.1.0",
            "  Java Version: 17.0.8, Eclipse Adoptium",
            "  Memory: 2147483648 bytes (17% free)");

    public FakeCrashEvent() {
        super("fake_crash", Act.FOUR, 10, 5, 8, 20 * 60 * 15);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, net.minecraft.server.level.ServerLevel level) {
        // Ложный краш не выпадает игроку, который только что играл (первая неделя мира)
        return memory.getDaysInWorld() >= 25;
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }
        int duration = 20 * 7;

        NetworkHandler.toPlayer(new S2CPlaySoundPacket(ModSounds.CRASH.getId(),
                player.getX(), player.getY(), player.getZ(), 0.6F, 0.7F), serverPlayer);
        NetworkHandler.toPlayer(new S2CFakeCrashPacket("The game crashed whilst ticking entity",
                STACK_TRACE, duration, Config.metaScreenshots()), serverPlayer);

        // Игра "возвращается" сама — без единого объяснения
        ScheduledTasks.schedule(level, duration, () -> NetworkHandler.toPlayer(new S2CCrashEndPacket(), serverPlayer));
    }
}
