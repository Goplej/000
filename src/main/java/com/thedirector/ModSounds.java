package com.thedirector;

import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.sounds.SoundEvent;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.registries.DeferredRegister;
import net.minecraftforge.registries.RegistryObject;

/**
 * Звуки мода.
 *
 * <p>Собственные ogg-файлы не используются: каждый звук мода объявлен в
 * {@code assets/thedirector/sounds.json} как ссылка на ванильное звуковое событие
 * ({@code "type": "event"}). Так мод не тянет за собой бинарные ассеты, но получает
 * свои собственные идентификаторы — например, чтобы отличить "шёпот режиссёра"
 * от обычного эндермена.</p>
 */
public final class ModSounds {

    public static final DeferredRegister<SoundEvent> SOUNDS =
            DeferredRegister.create(Registries.SOUND_EVENT, TheDirector.MOD_ID);

    /** Шёпот за спиной. */
    public static final RegistryObject<SoundEvent> WHISPER = register("whisper");
    /** Дыхание рядом. */
    public static final RegistryObject<SoundEvent> BREATH = register("breath");
    /** Шаги на крыше. */
    public static final RegistryObject<SoundEvent> ROOF_STEP = register("roof_step");
    /** Эхо собственных шагов во сне. */
    public static final RegistryObject<SoundEvent> STEP_ECHO = register("step_echo");
    /** Пустой гул сна. */
    public static final RegistryObject<SoundEvent> VOID_AMBIENT = register("void_ambient");
    /** Помехи. */
    public static final RegistryObject<SoundEvent> STATIC_NOISE = register("static_noise");
    /** Ложный краш. */
    public static final RegistryObject<SoundEvent> CRASH = register("crash");
    /** Тушение огня. */
    public static final RegistryObject<SoundEvent> EXTINGUISH = register("extinguish");
    /** Скрип дерева. */
    public static final RegistryObject<SoundEvent> WOOD_CREAK = register("wood_creak");
    /** Предмет "работает наоборот". */
    public static final RegistryObject<SoundEvent> REVERSE = register("reverse");
    /** Звук сундука. */
    public static final RegistryObject<SoundEvent> CHEST = register("chest");
    /** Гром без причины. */
    public static final RegistryObject<SoundEvent> THUNDER = register("thunder");

    private static RegistryObject<SoundEvent> register(String name) {
        return SOUNDS.register(name, () -> SoundEvent.createVariableRangeEvent(
                new ResourceLocation(TheDirector.MOD_ID, name)));
    }

    public static void register(IEventBus modBus) {
        SOUNDS.register(modBus);
    }

    private ModSounds() {
    }
}
