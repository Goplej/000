package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

/**
 * Конец ложного краша.
 *
 * <p>Отдельный пакет, а не таймер на клиенте: только сервер решает, когда "игра вернулась".</p>
 */
public class S2CCrashEndPacket {

    public S2CCrashEndPacket() {
    }

    public S2CCrashEndPacket(FriendlyByteBuf buf) {
        // данных нет
    }

    public void encode(FriendlyByteBuf buf) {
        // данных нет
    }

    public void handle() {
        ClientEffects.endFakeCrash();
    }
}
