package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

/**
 * Виньетка и туман.
 *
 * <p>Один пакет управляет сразу двумя эффектами, потому что в концепции мода они всегда
 * идут вместе: "мир сжимается".</p>
 */
public class S2CVignettePacket {

    private final float vignetteIntensity;
    private final float fogDistance;
    private final int durationTicks;

    public S2CVignettePacket(float vignetteIntensity, float fogDistance, int durationTicks) {
        this.vignetteIntensity = vignetteIntensity;
        this.fogDistance = fogDistance;
        this.durationTicks = durationTicks;
    }

    public S2CVignettePacket(FriendlyByteBuf buf) {
        this.vignetteIntensity = buf.readFloat();
        this.fogDistance = buf.readFloat();
        this.durationTicks = buf.readVarInt();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeFloat(vignetteIntensity);
        buf.writeFloat(fogDistance);
        buf.writeVarInt(durationTicks);
    }

    public void handle() {
        ClientEffects.setVignette(vignetteIntensity, fogDistance, durationTicks);
    }
}
