package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.CorruptionTags;
import net.minecraft.core.BlockPos;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.Container;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;

import java.util.Random;

/**
 * Возврат потерянного предмета.
 *
 * <p>Игрок выбросил вещь (или потерял её при смерти) — и через несколько дней находит
 * её там, где сам никогда не был: в чужом сундуке, за стеной, на крыше. Предмет
 * возвращается с испорченным именем: "он держал это в руках".</p>
 *
 * <p>Иногда именно возвращённый предмет становится ключом от сна.</p>
 */
public class LostItemReturnEvent extends DirectorEvent {

    private static final Random RANDOM = new Random();

    /** Имена, которые получает возвращённый предмет. */
    private static final String[] NAMES = {
            "\u0415\u0433\u043e \u0432\u0435\u0449\u044c",
            "\u0422\u0432\u043e\u0451?",
            "\u041e\u043d \u0432\u0435\u0440\u043d\u0443\u043b",
            "\u041e\u043d\u043e \u0435\u0449\u0451 \u0442\u0451\u043f\u043b\u043e\u0435",
            "\u041f\u043e\u043c\u043d\u0438\u0448\u044c?"
    };

    public LostItemReturnEvent() {
        super("lost_return", Act.THREE, 14, 3, 5, 20 * 60 * 10);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        if (!(player instanceof ServerPlayer serverPlayer)) {
            return;
        }
        ItemStack item = memory.pollLostItem();
        if (item.isEmpty()) {
            // Если игрок ничего не терял — вернём ему точную копию того, что у него есть
            item = memory.randomSnapshotItem(RANDOM);
        }
        if (item.isEmpty()) {
            return;
        }

        item = item.copyWithCount(Math.max(1, item.getCount()));
        item.setHoverName(Component.literal(NAMES[RANDOM.nextInt(NAMES.length)]));
        item.getOrCreateTag().putBoolean(CorruptionTags.CORRUPTED, true);

        // Иногда возвращённый предмет становится ключом от сна
        if (RANDOM.nextFloat() < 0.5F) {
            item.getOrCreateTag().putBoolean(CorruptionTags.DREAM_KEY, true);
        }

        Container container = Search.findContainer(level, player.blockPosition(), 12);
        if (container != null && RANDOM.nextBoolean()) {
            int slot = findEmptySlot(container);
            container.setItem(slot, item);
            container.setChanged();
            return;
        }

        BlockPos spot = Search.strangeSpot(level, player.blockPosition(), 10, RANDOM);
        if (spot == null) {
            spot = player.blockPosition().above();
        }
        ItemEntity entity = new ItemEntity(level, spot.getX() + 0.5D, spot.getY() + 0.3D, spot.getZ() + 0.5D, item);
        entity.setNeverPickUp();
        level.addFreshEntity(entity);
    }

    /** Свободный слот или случайный, если свободных нет. */
    private int findEmptySlot(Container container) {
        for (int i = 0; i < container.getContainerSize(); i++) {
            if (container.getItem(i).isEmpty()) {
                return i;
            }
        }
        return RANDOM.nextInt(container.getContainerSize());
    }
}
