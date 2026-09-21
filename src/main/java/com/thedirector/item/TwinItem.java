package com.thedirector.item;

import com.thedirector.util.CorruptionTags;
import net.minecraft.network.chat.Component;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.TooltipFlag;
import net.minecraft.world.level.Level;

import javax.annotation.Nullable;
import java.util.List;

/**
 * Предмет-двойник.
 *
 * <p>Система порчи подменяет им одну из "не-ванильных" вещей игрока. Двойник визуально
 * остаётся тем же предметом: имя берётся из NBT (имя оригинала), поэтому в инвентаре он
 * выглядит как ваша вещь. Отличие видно только в момент использования: двойник "работает
 * наоборот" — вместо пользы даёт эффект отравления, и звучит это задом наперёд.</p>
 */
public class TwinItem extends Item {

    public TwinItem(Properties properties) {
        super(properties);
    }

    @Override
    public Component getName(ItemStack stack) {
        // Если двойник был создан системой порчи — показываем имя "украденной" вещи
        if (stack.hasTag() && stack.getTag().contains(CorruptionTags.STOLEN_NAME)) {
            return Component.literal(stack.getTag().getString(CorruptionTags.STOLEN_NAME));
        }
        return super.getName(stack);
    }

    @Override
    public void appendHoverText(ItemStack stack, @Nullable Level level, List<Component> tooltip, TooltipFlag flag) {
        tooltip.add(Component.translatable("item.thedirector.twin_item.hover"));
    }
}
