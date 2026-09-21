package com.thedirector.test;

import com.thedirector.Config;
import com.thedirector.ModItems;
import com.thedirector.TheDirector;
import com.thedirector.director.EventDirector;
import com.thedirector.dream.DreamDimension;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.dimension.DimensionType;
import net.minecraftforge.event.server.ServerStartedEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;
import net.minecraftforge.registries.ForgeRegistries;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Встроенный самотест мода.
 *
 * <p>Запускается только при переменной окружения {@code THEDIRECTOR_SELFTEST=1} и
 * существует для того, чтобы проверять мод на настоящем сервере: зарегистрировано ли
 * измерение сна, читается ли конфиг, работают ли планировщик задач, capability, реестр
 * событий и сетевой канал.</p>
 *
 * <p>В обычной игре класс не делает ничего: без переменной окружения он молча выходит
 * из обработчика.</p>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class SelfTest {

    /** Имя переменной окружения, включающей самотест. */
    public static final String ENV_FLAG = "THEDIRECTOR_SELFTEST";

    private SelfTest() {
    }

    @SubscribeEvent
    public static void onServerStarted(ServerStartedEvent event) {
        if (!"1".equals(System.getenv(ENV_FLAG))) {
            return;
        }
        MinecraftServer server = event.getServer();
        List<String> report = new ArrayList<>();
        AtomicBoolean failed = new AtomicBoolean(false);

        // --- 1. Синхронные проверки ---
        check(report, failed, "измерение сна зарегистрировано датапаком",
                () -> server.getLevel(DreamDimension.DREAM_KEY) != null);

        check(report, failed, "тип измерения сна корректен (кровать не работает, время стоит)",
                () -> {
                    ServerLevel dream = server.getLevel(DreamDimension.DREAM_KEY);
                    if (dream == null) {
                        return false;
                    }
                    DimensionType type = dream.dimensionType();
                    return !type.bedWorks() && type.fixedTime().isPresent()
                            && type.fixedTime().getAsLong() == DreamDimension.FROZEN_TIME;
                });

        check(report, failed, "в измерении сна нет естественного спавна мобов",
                () -> {
                    ServerLevel dream = server.getLevel(DreamDimension.DREAM_KEY);
                    return dream != null && !dream.dimensionType().natural();
                });

        check(report, failed, "конфиг читается (eventFrequency > 0)", () -> Config.eventFrequency() > 0);
        check(report, failed, "реестр событий режиссёра заполнен (>= 13)",
                () -> EventDirector.all().size() >= 13);
        check(report, failed, "все события имеют положительный вес",
                () -> EventDirector.all().stream().allMatch(e -> e.weight() > 0));
        check(report, failed, "сетевой канал создан", () -> NetworkHandler.CHANNEL != null);
        check(report, failed, "предмет-двойник зарегистрирован",
                () -> ForgeRegistries.ITEMS.containsKey(new ResourceLocation(TheDirector.MOD_ID, "twin_item")));
        check(report, failed, "звуки мода зарегистрированы (12 штук)",
                () -> ForgeRegistries.SOUND_EVENTS.containsKey(new ResourceLocation(TheDirector.MOD_ID, "whisper"))
                        && ForgeRegistries.SOUND_EVENTS.containsKey(new ResourceLocation(TheDirector.MOD_ID, "crash"))
                        && ForgeRegistries.SOUND_EVENTS.getKeys().size() > 0);
        check(report, failed, "измерение сна есть в реестре измерений сервера",
                () -> server.levelKeys().contains(DreamDimension.DREAM_KEY));
        check(report, failed, "реестр измерений содержит ванильный overworld",
                () -> server.registryAccess().registryOrThrow(Registries.DIMENSION_TYPE).containsKey(
                        new ResourceLocation(TheDirector.MOD_ID, "dream_type")));

        check(report, failed, "память игрока сериализуется и читается обратно", () -> {
            PlayerMemory memory = new PlayerMemory();
            memory.setSafetyLevel(17);
            memory.setDreadLevel(9);
            memory.setCurrentAct(3);
            memory.setDaysInWorld(21);
            memory.setLastSeenPos(new net.minecraft.core.BlockPos(10, 64, -30));
            memory.rememberLostItem(new net.minecraft.world.item.ItemStack(ModItems.TWIN_ITEM.get(), 3));
            memory.pushHistory("whisper");

            PlayerMemory restored = new PlayerMemory();
            restored.deserializeNBT(memory.serializeNBT());
            return restored.getSafetyLevel() == 17
                    && restored.getDreadLevel() == 9
                    && restored.getCurrentAct() == 3
                    && restored.getDaysInWorld() == 21
                    && restored.getLostItems().size() == 1
                    && restored.getEventsHistory().contains("whisper")
                    && restored.getLastSeenPos() != null;
        });

        check(report, failed, "система порчи предметов находит ключ по NBT", () -> {
            net.minecraft.world.item.ItemStack stack = new net.minecraft.world.item.ItemStack(ModItems.TWIN_ITEM.get());
            stack.getOrCreateTag().putBoolean(com.thedirector.util.CorruptionTags.DREAM_KEY, true);
            return com.thedirector.corruption.ItemCorruptionSystem.isDreamKey(stack);
        });

        // --- 2. Асинхронная проверка планировщика (нужно несколько тиков) ---
        AtomicBoolean schedulerRan = new AtomicBoolean(false);
        ServerLevel overworld = server.overworld();
        ScheduledTasks.schedule(overworld, 20, () -> schedulerRan.set(true));

        ScheduledTasks.schedule(overworld, 45, () -> {
            check(report, failed, "планировщик отложенных задач выполняет задачи", schedulerRan::get);
            StringBuilder builder = new StringBuilder();
            builder.append("\n===== THEDIRECTOR SELFTEST =====\n");
            report.forEach(line -> builder.append(line).append('\n'));
            builder.append(failed.get() ? "RESULT: FAILED\n" : "RESULT: OK\n");
            builder.append("================================\n");
            TheDirector.LOGGER.info(builder.toString());
            TheDirector.LOGGER.info("[The Director] SELFTEST_DONE {}", failed.get() ? "FAILED" : "OK");
            server.halt(false);
        });
    }

    private interface Check {
        boolean run() throws Exception;
    }

    private static void check(List<String> report, AtomicBoolean failed, String name, Check check) {
        boolean result;
        try {
            result = check.run();
        } catch (Exception | LinkageError exception) {
            result = false;
            report.add("  [ERR] " + name + " -> " + exception);
        }
        if (!result) {
            failed.set(true);
        }
        report.add((result ? "  [OK]  " : "  [FAIL] ") + name);
    }
}
