package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

/**
 * Тряска камеры.
 *
 * <p>"Удар" в драматургии мода: тело реагирует раньше, чем игрок понимает, что произошло.</p>
 */
public class S2CShakeCameraPacket {

    private final float intensity;
    private final int durationTicks;

    public S2CShakeCameraPacket(float intensity, int durationTicks) {
        this.intensity = intensity;
        this.durationTicks = durationTicks;
    }

    public S2CShakeCameraPacket(FriendlyByteBuf buf) {
        this.intensity = buf.readFloat();
        this.durationTicks = buf.readVarInt();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeFloat(intensity);
        buf.writeVarInt(durationTicks);
    }

    public void handle() {
        ClientEffects.shake(intensity, durationTicks);
    }
}
