package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;
import net.minecraft.resources.ResourceLocation;

/**
 * Звук "на игроке".
 *
 * <p>В отличие от {@link S2CPlaySoundPacket}, звук не привязан к точке мира: он звучит
 * там, где игрок находится в момент получения пакета. Используется для эффектов
 * присутствия рядом (дыхание, шёпот в упор).</p>
 */
public class S2CSoundAtPlayerPacket {

    private final ResourceLocation soundId;
    private final float volume;
    private final float pitch;

    public S2CSoundAtPlayerPacket(ResourceLocation soundId, float volume, float pitch) {
        this.soundId = soundId;
        this.volume = volume;
        this.pitch = pitch;
    }

    public S2CSoundAtPlayerPacket(FriendlyByteBuf buf) {
        this.soundId = buf.readResourceLocation();
        this.volume = buf.readFloat();
        this.pitch = buf.readFloat();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeResourceLocation(soundId);
        buf.writeFloat(volume);
        buf.writeFloat(pitch);
    }

    public void handle() {
        ClientEffects.playSoundAtSelf(soundId, volume, pitch);
    }
}
