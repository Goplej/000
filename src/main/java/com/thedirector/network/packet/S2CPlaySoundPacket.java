package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;
import net.minecraft.resources.ResourceLocation;

/**
 * Позиционный звук.
 *
 * <p>Нужен там, где обычный {@code level.playSound} не даёт контроля: например, когда
 * звук должен звучать строго для одного игрока или с точной задержкой на клиенте.</p>
 */
public class S2CPlaySoundPacket {

    private final ResourceLocation soundId;
    private final double x;
    private final double y;
    private final double z;
    private final float volume;
    private final float pitch;

    public S2CPlaySoundPacket(ResourceLocation soundId, double x, double y, double z, float volume, float pitch) {
        this.soundId = soundId;
        this.x = x;
        this.y = y;
        this.z = z;
        this.volume = volume;
        this.pitch = pitch;
    }

    public S2CPlaySoundPacket(FriendlyByteBuf buf) {
        this.soundId = buf.readResourceLocation();
        this.x = buf.readDouble();
        this.y = buf.readDouble();
        this.z = buf.readDouble();
        this.volume = buf.readFloat();
        this.pitch = buf.readFloat();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeResourceLocation(soundId);
        buf.writeDouble(x);
        buf.writeDouble(y);
        buf.writeDouble(z);
        buf.writeFloat(volume);
        buf.writeFloat(pitch);
    }

    public void handle() {
        ClientEffects.playPositionalSound(soundId, x, y, z, volume, pitch);
    }
}
