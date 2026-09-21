package com.thedirector.corruption;

import com.thedirector.ModItems;
import com.thedirector.TheDirector;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.CorruptionTags;
import com.thedirector.util.MetaLayer;
import com.thedirector.util.Texts;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.item.ItemStack;
import net.minecraftforge.registries.ForgeRegistries;

import java.util.ArrayList;
import java.util.List;

/**
 * Система порчи предметов.
 *
 * <p>Работает только в момент входа в сон и только с "не-ванильными" предметами
 * (модовые, кастомные — всё, у чего namespace не {@code minecraft}). Ванильные предметы
 * остаются нетронутыми: мод не ломает базовую игру. Если в инвентаре нет ни одного
 * модового предмета, система аккуратно портит один-два ванильных — иначе механика
 * была бы невидимой в чистом мире.</p>
 *
 * <p>Четыре операции, применяемые по одной за вход:</p>
 * <ol>
 *   <li>NBT-имя меняется на жуткое;</li>
 *   <li>предмет дублируется в другой слот;</li>
 *   <li>предмет заменяется на "двойника" (визуально тот же, ID другой);</li>
 *   <li>предмет работает наоборот.</li>
 * </ol>
 *
 * <p>Предметы <b>никогда не исчезают полностью</b>: подменённая вещь уходит в память
 * режиссёра и возвращается игроку позже в другом месте.</p>
 */
public final class ItemCorruptionSystem {

    /** Диапазон слотов основной инвентаря (0-8 хотбар, 9-35 инвентарь). */
    private static final int MAIN_SLOTS = 36;

    private static final RandomSource RANDOM = RandomSource.create();

    private ItemCorruptionSystem() {
    }

    /**
     * Перед сном: пометить одну вещь как "ключ" — именно через неё игрок сможет выйти.
     * Ключ выбирается из не-ванильных предметов, чтобы находка была неочевидной.
     */
    public static void prepareKey(ServerPlayer player, PlayerMemory memory) {
        List<Integer> targets = corruptibleSlots(player);
        if (targets.isEmpty()) {
            targets = occupiedSlots(player);
        }
        if (targets.isEmpty()) {
            return;
        }
        int slot = targets.get(RANDOM.nextInt(targets.size()));
        ItemStack stack = player.getInventory().getItem(slot).copy();
        stack.getOrCreateTag().putBoolean(CorruptionTags.DREAM_KEY, true);
        stack.setHoverName(Component.literal(Texts.corruptedItemName(RANDOM)));
        player.getInventory().setItem(slot, stack);
        MetaLayer.note(player, "DREAM_KEY slot=" + slot + " item=" + stack.getItem());
    }

    /**
     * Применить порчу предметов при входе в сон.
     * Все четыре операции выполняются по одной, без исключений.
     */
    public static void corrupt(ServerPlayer player, PlayerMemory memory) {
        List<Integer> modded = corruptibleSlots(player);
        boolean onlyVanilla = modded.isEmpty();
        List<Integer> targets = onlyVanilla ? occupiedSlots(player) : modded;
        if (targets.isEmpty()) {
            return;
        }

        renameOne(player, targets);
        duplicateOne(player, targets);
        replaceWithTwin(player, memory, targets);
        invertOne(player, targets);

        MetaLayer.note(player, "CORRUPTED items: modded=" + modded.size() + " vanillaFallback=" + onlyVanilla);
    }

    /** 1. NBT-имя меняется на жуткое. */
    private static void renameOne(ServerPlayer player, List<Integer> targets) {
        int slot = targets.get(RANDOM.nextInt(targets.size()));
        ItemStack stack = player.getInventory().getItem(slot).copy();
        if (stack.isEmpty()) {
            return;
        }
        stack.setHoverName(Component.literal(Texts.corruptedItemName(RANDOM)));
        stack.getOrCreateTag().putBoolean(CorruptionTags.CORRUPTED, true);
        player.getInventory().setItem(slot, stack);
    }

    /** 2. Предмет дублируется в другой слот: игрок видит две одинаковые вещи. */
    private static void duplicateOne(ServerPlayer player, List<Integer> targets) {
        int slot = targets.get(RANDOM.nextInt(targets.size()));
        ItemStack stack = player.getInventory().getItem(slot);
        if (stack.isEmpty()) {
            return;
        }
        int empty = firstEmptySlot(player);
        if (empty < 0) {
            // Свободного места нет — кладём копию поверх "похожего" предмета
            empty = targets.get(RANDOM.nextInt(targets.size()));
        }
        if (empty == slot) {
            return;
        }
        ItemStack copy = stack.copy();
        copy.getOrCreateTag().putBoolean(CorruptionTags.DUPLICATE, true);
        player.getInventory().setItem(empty, copy);
    }

    /**
     * 3. Предмет заменяется на двойника: визуально та же вещь, но другой ID.
     * Оригинал не исчезает — он уходит в память режиссёра.
     */
    private static void replaceWithTwin(ServerPlayer player, PlayerMemory memory, List<Integer> targets) {
        int slot = targets.get(RANDOM.nextInt(targets.size()));
        ItemStack original = player.getInventory().getItem(slot).copy();
        if (original.isEmpty() || original.is(ModItems.TWIN_ITEM.get())) {
            return;
        }
        String displayName = original.getHoverName().getString();

        ItemStack twin = new ItemStack(ModItems.TWIN_ITEM.get(), Math.max(1, Math.min(original.getCount(), 16)));
        twin.getOrCreateTag().putString(CorruptionTags.STOLEN_NAME, displayName);
        twin.getOrCreateTag().putBoolean(CorruptionTags.CORRUPTED, true);

        player.getInventory().setItem(slot, twin);
        // Оригинал не теряется: он вернётся игроку позже, в другом месте
        memory.rememberLostItem(original);
    }

    /** 4. Предмет начинает работать наоборот. */
    private static void invertOne(ServerPlayer player, List<Integer> targets) {
        int slot = targets.get(RANDOM.nextInt(targets.size()));
        ItemStack stack = player.getInventory().getItem(slot).copy();
        if (stack.isEmpty() || stack.is(ModItems.TWIN_ITEM.get())) {
            return;
        }
        stack.getOrCreateTag().putBoolean(CorruptionTags.INVERTED, true);
        player.getInventory().setItem(slot, stack);
    }

    // ------------------------------------------------------------ вспомогательное

    /**
     * Слоты с не-ванильными предметами (namespace != minecraft).
     *
     * <p>Ключ от сна исключается всегда: иначе порча могла бы заменить или переименовать
     * единственный предмет, через который игрок выходит из сна.</p>
     */
    public static List<Integer> corruptibleSlots(ServerPlayer player) {
        List<Integer> slots = new ArrayList<>();
        for (int slot = 0; slot < MAIN_SLOTS; slot++) {
            ItemStack stack = player.getInventory().getItem(slot);
            if (stack.isEmpty() || stack.is(ModItems.TWIN_ITEM.get()) || isDreamKey(stack)) {
                continue;
            }
            ResourceLocation id = ForgeRegistries.ITEMS.getKey(stack.getItem());
            if (id != null && !"minecraft".equals(id.getNamespace())) {
                slots.add(slot);
            }
        }
        return slots;
    }

    /** Все занятые слоты основной инвентаря (кроме ключа сна). */
    public static List<Integer> occupiedSlots(ServerPlayer player) {
        List<Integer> slots = new ArrayList<>();
        for (int slot = 0; slot < MAIN_SLOTS; slot++) {
            ItemStack stack = player.getInventory().getItem(slot);
            if (!stack.isEmpty() && !stack.getOrCreateTag().getBoolean(CorruptionTags.DREAM_KEY)) {
                slots.add(slot);
            }
        }
        return slots;
    }

    private static int firstEmptySlot(ServerPlayer player) {
        for (int slot = 0; slot < MAIN_SLOTS; slot++) {
            if (player.getInventory().getItem(slot).isEmpty()) {
                return slot;
            }
        }
        return -1;
    }

    /** Сколько предметов испорчено прямо сейчас (для отчёта/логов). */
    public static int countCorrupted(ServerPlayer player) {
        int count = 0;
        for (int slot = 0; slot < MAIN_SLOTS; slot++) {
            ItemStack stack = player.getInventory().getItem(slot);
            if (!stack.isEmpty() && stack.getOrCreateTag().getBoolean(CorruptionTags.CORRUPTED)) {
                count++;
            }
        }
        return count;
    }

    /** Проверка: предмет помечен как ключ от сна. */
    public static boolean isDreamKey(ItemStack stack) {
        return !stack.isEmpty() && stack.getOrCreateTag().getBoolean(CorruptionTags.DREAM_KEY);
    }

    /** Проверка: предмет работает наоборот. */
    public static boolean isInverted(ItemStack stack) {
        return !stack.isEmpty() && stack.getOrCreateTag().getBoolean(CorruptionTags.INVERTED);
    }

    /** Записать в лог (для отладки). */
    public static void logSummary(ServerPlayer player) {
        TheDirector.LOGGER.debug("[The Director] Испорченных предметов у {}: {}",
                player.getGameProfile().getName(), countCorrupted(player));
    }
}
