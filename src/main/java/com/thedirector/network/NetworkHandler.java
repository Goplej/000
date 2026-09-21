package com.thedirector.network;

import com.thedirector.TheDirector;
import com.thedirector.network.packet.S2CChatLinePacket;
import com.thedirector.network.packet.S2CCrashEndPacket;
import com.thedirector.network.packet.S2CFOVPacket;
import com.thedirector.network.packet.S2CFakeCrashPacket;
import com.thedirector.network.packet.S2CPlaySoundPacket;
import com.thedirector.network.packet.S2CShakeCameraPacket;
import com.thedirector.network.packet.S2CSoundAtPlayerPacket;
import com.thedirector.network.packet.S2CVignettePacket;
import com.thedirector.network.packet.S2CWindowTitlePacket;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerPlayer;
import net.minecraftforge.network.NetworkDirection;
import net.minecraftforge.network.NetworkRegistry;
import net.minecraftforge.network.PacketDistributor;
import net.minecraftforge.network.simple.SimpleChannel;

/**
 * Сетевой канал мода (Forge 1.20.1).
 *
 * <p>Пакеты идут только в одну сторону — от сервера к клиенту. Клиент не отправляет
 * режиссёру ничего: мод не спрашивает игрока ни о чём и не принимает от него команд.
 * Именно поэтому все эффекты — это "приказы" сервера, которые клиент обязан выполнить.</p>
 *
 * <p>Канал терпим к версиям: если у игрока нет мода, сервер просто не отправляет эффекты.</p>
 */
public final class NetworkHandler {

    /** Версия протокола. */
    public static final String PROTOCOL_VERSION = "1";

    private static int nextId;

    public static final SimpleChannel CHANNEL = NetworkRegistry.newSimpleChannel(
            new ResourceLocation(TheDirector.MOD_ID, "main"),
            () -> PROTOCOL_VERSION,
            version -> true,
            version -> true);

    /** Регистрация всех пакетов (вызывается в common setup). */
    public static void register() {
        CHANNEL.messageBuilder(S2CPlaySoundPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CPlaySoundPacket::encode)
                .decoder(S2CPlaySoundPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CShakeCameraPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CShakeCameraPacket::encode)
                .decoder(S2CShakeCameraPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CFOVPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CFOVPacket::encode)
                .decoder(S2CFOVPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CWindowTitlePacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CWindowTitlePacket::encode)
                .decoder(S2CWindowTitlePacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CVignettePacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CVignettePacket::encode)
                .decoder(S2CVignettePacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CChatLinePacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CChatLinePacket::encode)
                .decoder(S2CChatLinePacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CFakeCrashPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CFakeCrashPacket::encode)
                .decoder(S2CFakeCrashPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CCrashEndPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CCrashEndPacket::encode)
                .decoder(S2CCrashEndPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();

        CHANNEL.messageBuilder(S2CSoundAtPlayerPacket.class, nextId++, NetworkDirection.PLAY_TO_CLIENT)
                .encoder(S2CSoundAtPlayerPacket::encode)
                .decoder(S2CSoundAtPlayerPacket::new)
                .consumerMainThread((message, context) -> message.handle())
                .add();
    }

    /** Отправить пакет конкретному игроку. Если клиент не готов — молча пропустить. */
    public static void toPlayer(Object packet, ServerPlayer player) {
        try {
            send(packet, player);
        } catch (RuntimeException | LinkageError exception) {
            TheDirector.LOGGER.debug("[The Director] Клиент не принял пакет: {}", exception.getMessage());
        }
    }

    private static <MSG> void send(MSG packet, ServerPlayer player) {
        CHANNEL.send(packet, PacketDistributor.PLAYER.with(() -> player));
    }
}
