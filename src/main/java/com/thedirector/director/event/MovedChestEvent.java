package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.event.support.Search;
import com.thedirector.memory.PlayerMemory;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.Container;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;

/**
 * Перекладывание вещей в сундуке игрока.
 *
 * <p>Мод не крадёт вещи: он меняет их местами. Игрок ищет предмет там, где он лежал,
 * не находит и начинает сомневаться в собственной памяти. Это работает лучше любой пропажи.</p>
 *
 * <p>В акте 3 и выше с некоторой вероятностью один предмет исчезает из сундука — и
 * возвращается позже в другом месте (см. {@link LostItemReturnEvent}).</p>
 */
public class MovedChestEvent extends DirectorEvent {

    public MovedChestEvent() {
        super("moved_chest", Act.TWO, 16, 2, 4, 20 * 60 * 4);
    }

    @Override
    public boolean canRun(ServerPlayer player, PlayerMemory memory, net.minecraft.server.level.ServerLevel level) {
        return Search.findContainer(level, player.blockPosition(), 10) != null;
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        Container container = Search.findContainer(level, player.blockPosition(), 10);
        if (container == null || container.getContainerSize() < 2) {
            return;
        }
        Act act = Act.of(memory.getCurrentAct());
        int size = container.getContainerSize();

        // Перестановка двух предметов (или предмета в пустой слот — тоже заметно)
        int first = level.random.nextInt(size);
        int second = level.random.nextInt(size);
        if (first == second) {
            second = (second + 1) % size;
        }
        ItemStack a = container.getItem(first).copy();
        ItemStack b = container.getItem(second).copy();
        container.setItem(first, b);
        container.setItem(second, a);
        container.setChanged();

        // Сундук "хлопает" сам по себе — игрок слышит это из дома
        playSound(level, player.getX(), player.getY() + 1.0D, player.getZ(),
                com.thedirector.ModSounds.CHEST.get(), 0.4F, 0.8F);

        if (act.number() >= 3 && !a.isEmpty() && level.random.nextFloat() < 0.4F) {
            // Забираем предмет из памяти и вернём его позже в странном месте
            memory.rememberLostItem(a);
        }
    }
}
