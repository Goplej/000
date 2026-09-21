package com.thedirector.util;

import com.thedirector.TheDirector;
import com.thedirector.dream.DreamDimension;
import com.thedirector.dream.DreamReplicator;
import net.minecraft.server.level.ServerLevel;
import net.minecraftforge.event.TickEvent;
import net.minecraftforge.event.server.ServerStoppedEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;

/**
 * Служебный тик: отложенные задачи и достройка сна.
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class TickHandler {

    private TickHandler() {
    }

    @SubscribeEvent
    public static void onLevelTick(TickEvent.LevelTickEvent event) {
        if (event.phase != TickEvent.Phase.END || !(event.level instanceof ServerLevel level)) {
            return;
        }
        ScheduledTasks.tick(level);
        if (DreamDimension.isDream(level)) {
            DreamReplicator.tick(level);
        }
    }

    /** Остановка сервера: чистим очереди, иначе задачи утекут в следующий мир. */
    @SubscribeEvent
    public static void onServerStopped(ServerStoppedEvent event) {
        ScheduledTasks.clear();
        DreamReplicator.clear();
        TheDirector.LOGGER.info("[The Director] Сервер остановлен. Он подождёт.");
    }
}
