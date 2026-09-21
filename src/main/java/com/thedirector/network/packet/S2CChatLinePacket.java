package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;
import net.minecraft.network.chat.Component;

/**
 * Фейковая строка чата.
 *
 * <p>Пакет существует, потому что отправить сообщение "от себя" обычным способом нельзя:
 * сервер обязан показать ник отправителя. Здесь ник подставляет сам мод.</p>
 */
public class S2CChatLinePacket {

    private final Component text;

    public S2CChatLinePacket(Component text) {
        this.text = text;
    }

    public S2CChatLinePacket(FriendlyByteBuf buf) {
        this.text = buf.readComponent();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeComponent(text);
    }

    public void handle() {
        ClientEffects.addChatLine(text);
    }
}
