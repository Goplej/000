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
 * <p>Все двенадцать звуков — собственные: они синтезируются программно
 * (см. {@code tools/generate_sounds.py}) и лежат в
 * {@code assets/thedirector/sounds/*.ogg} в формате Ogg Vorbis. Описания — в
 * {@code assets/thedirector/sounds.json}.</p>
 *
 * <p>Субтитры к звукам намеренно не заданы: подпись вида "Шёпот" объяснила бы игроку
 * то, что мод объяснять не должен.</p>
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
