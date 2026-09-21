package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

/**
 * Подмена заголовка окна.
 *
 * <p>Заголовок — это то, что игрок не видит, пока не свернёт игру. Именно поэтому мод
 * меняет его: игрок возвращается к рабочему столу и обнаруживает, что окно называется
 * иначе. Пустая строка означает "вернуть как было".</p>
 */
public class S2CWindowTitlePacket {

    private final String title;
    private final int holdTicks;

    public S2CWindowTitlePacket(String title, int holdTicks) {
        this.title = title;
        this.holdTicks = holdTicks;
    }

    public S2CWindowTitlePacket(FriendlyByteBuf buf) {
        this.title = buf.readUtf(256);
        this.holdTicks = buf.readVarInt();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeUtf(title, 256);
        buf.writeVarInt(holdTicks);
    }

    public void handle() {
        ClientEffects.setWindowTitle(title, holdTicks);
    }
}
