package com.thedirector;

import com.thedirector.item.TwinItem;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.world.item.CreativeModeTab;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.registries.DeferredRegister;
import net.minecraftforge.registries.ForgeRegistries;
import net.minecraftforge.registries.RegistryObject;

/**
 * Регистрация предметов и вкладки креатива.
 *
 * <p>Мод добавляет ровно один предмет — "двойника" ({@code thedirector:twin_item}).
 * Он создаётся только системой порчи предметов: игрок не может его скрафтить.</p>
 */
public final class ModItems {

    public static final DeferredRegister<Item> ITEMS =
            DeferredRegister.create(ForgeRegistries.ITEMS, TheDirector.MOD_ID);

    public static final DeferredRegister<CreativeModeTab> TABS =
            DeferredRegister.create(Registries.CREATIVE_MODE_TAB, TheDirector.MOD_ID);

    /** Двойник: визуально похож на вашу вещь, но ID другой. */
    public static final RegistryObject<Item> TWIN_ITEM = ITEMS.register("twin_item",
            () -> new TwinItem(new Item.Properties().stacksTo(16)));

    /** Отдельная вкладка креатива — чтобы предмет можно было проверить вручную. */
    public static final RegistryObject<CreativeModeTab> MAIN_TAB = TABS.register("main",
            () -> CreativeModeTab.builder()
                    .title(Component.translatable("itemGroup.thedirector"))
                    .icon(() -> new ItemStack(TWIN_ITEM.get()))
                    .displayItems((parameters, output) -> output.accept(TWIN_ITEM.get()))
                    .build());

    public static void register(IEventBus modBus) {
        ITEMS.register(modBus);
        TABS.register(modBus);
    }

    private ModItems() {
    }
}
