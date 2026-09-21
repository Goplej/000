package com.thedirector.network.packet;

import com.thedirector.client.ClientEffects;
import net.minecraft.network.FriendlyByteBuf;

import java.util.ArrayList;
import java.util.List;

/**
 * Ложный краш.
 *
 * <p>Самый тяжёлый приём мода: игрок видит знакомый экран смерти игры, читает стек вызовов
 * — и через несколько секунд экран исчезает. Мир возвращается. Объяснений нет.</p>
 */
public class S2CFakeCrashPacket {

    private final String header;
    private final List<String> stackTrace;
    private final int durationTicks;
    private final boolean takeScreenshot;

    public S2CFakeCrashPacket(String header, List<String> stackTrace, int durationTicks, boolean takeScreenshot) {
        this.header = header;
        this.stackTrace = stackTrace;
        this.durationTicks = durationTicks;
        this.takeScreenshot = takeScreenshot;
    }

    public S2CFakeCrashPacket(FriendlyByteBuf buf) {
        this.header = buf.readUtf(512);
        int size = buf.readVarInt();
        List<String> lines = new ArrayList<>(size);
        for (int i = 0; i < size; i++) {
            lines.add(buf.readUtf(512));
        }
        this.stackTrace = lines;
        this.durationTicks = buf.readVarInt();
        this.takeScreenshot = buf.readBoolean();
    }

    public void encode(FriendlyByteBuf buf) {
        buf.writeUtf(header, 512);
        buf.writeVarInt(stackTrace.size());
        for (String line : stackTrace) {
            buf.writeUtf(line, 512);
        }
        buf.writeVarInt(durationTicks);
        buf.writeBoolean(takeScreenshot);
    }

    public void handle() {
        ClientEffects.fakeCrash(header, stackTrace, durationTicks, takeScreenshot);
    }
}
