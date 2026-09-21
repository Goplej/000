package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

/**
 * Изменение FOV.
 *
 * <p>Сужение поля зрения — самый честный способ показать игроку, что он больше не
 * контролирует ситуацию: он видит меньше, но не может сказать почему.</p>
 */
public class S2CFOVPacket {

    private final float multiplier;
    private final int durationTicks;

    public S2CFOVPacket(float multiplier, int durationTicks) {
        this.multiplier = multiplier;
        this.durationTicks = durationTicks;
    }

    public S2CFOVPacket(FriendlyByteBuf buf) {
        this.multiplier = buf.readFloat();
        this.durationTicks = buf.readVarInt();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeFloat(multiplier);
        buf.writeVarInt(durationTicks);
    }

    public void handle() {
        ClientEffects.setFov(multiplier, durationTicks);
    }
}
