package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.network.NetworkHandler;
import com.thedirector.network.packet.S2CChatLinePacket;
import com.thedirector.util.Texts;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Фейковое сообщение в чат.
 *
 * <p>В акте 2 это безликая строка, которую легко списать на глюк. В акте 3 и выше
 * сообщение приходит "от самого игрока" — он видит свой ник и фразу, которую не писал.
 * Именно этот приём ломает доверие к интерфейсу.</p>
 */
public class ChatMessageEvent extends DirectorEvent {

    public ChatMessageEvent() {
        super("chat_message", Act.TWO, 18, 2, 3, 20 * 60 * 6);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }
        Act act = Act.of(memory.getCurrentAct());

        if (act.number() >= 3 && level.random.nextFloat() < 0.7F) {
            // Сообщение "от игрока": ник + обрывок фразы
            Component body = Texts.randomMessage(act, level.random);
            Component fake = Component.literal("<" + serverPlayer.getGameProfile().getName() + "> ")
                    .append(body);
            NetworkHandler.toPlayer(new S2CChatLinePacket(fake), serverPlayer);
        } else {
            NetworkHandler.toPlayer(new S2CChatLinePacket(Texts.randomMessage(act, level.random)), serverPlayer);
        }

        // В акте 4 вслед за сообщением приходит "ответ" от мира
        if (act == Act.FOUR) {
            NetworkHandler.toPlayer(new S2CChatLinePacket(Texts.glitchMessage(level.random)), serverPlayer);
        }
    }
}
