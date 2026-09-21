package com.thedirector;

import com.mojang.logging.LogUtils;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import net.minecraftforge.common.capabilities.RegisterCapabilitiesEvent;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.fml.ModLoadingContext;
import net.minecraftforge.fml.common.Mod;
import net.minecraftforge.fml.config.ModConfig;
import net.minecraftforge.fml.event.lifecycle.FMLCommonSetupEvent;
import net.minecraftforge.fml.javafmlmod.FMLJavaModLoadingContext;
import org.slf4j.Logger;

/**
 * Точка входа мода "The Director".
 *
 * <p>Мод не содержит ни ИИ, ни обращений к сети, ни LLM: вся "режиссура" — это
 * детерминированная машина состояний (см. {@link com.thedirector.director.Director}),
 * которая читает память игрока ({@link PlayerMemory}) и запускает заранее написанные
 * события ({@link com.thedirector.director.event.DirectorEvent}).</p>
 *
 * <p>Все игровые правила применяются на сервере, клиент получает только визуальные
 * и звуковые команды через {@link NetworkHandler}.</p>
 */
@Mod(TheDirector.MOD_ID)
public class TheDirector {

    /** Идентификатор мода. */
    public static final String MOD_ID = "thedirector";

    /** Общий логгер мода. */
    public static final Logger LOGGER = LogUtils.getLogger();

    public TheDirector() {
        IEventBus modBus = FMLJavaModLoadingContext.get().getModEventBus();

        // Конфиг читается и на клиенте, и на сервере (COMMON)
        ModLoadingContext.get().registerConfig(ModConfig.Type.COMMON, Config.SPEC);

        // Регистрация контента
        ModItems.register(modBus);
        ModSounds.register(modBus);

        modBus.addListener(this::onCommonSetup);
        modBus.addListener(TheDirector::onRegisterCapabilities);

        LOGGER.info("[The Director] Загружается. Он уже смотрит.");
    }

    private void onCommonSetup(FMLCommonSetupEvent event) {
        // Сетевые пакеты регистрируются в общем потоке загрузки
        event.enqueueWork(NetworkHandler::register);
    }

    private static void onRegisterCapabilities(RegisterCapabilitiesEvent event) {
        event.register(PlayerMemory.class);
    }
}
