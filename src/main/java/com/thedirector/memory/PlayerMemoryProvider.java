package com.thedirector.memory;

import net.minecraft.core.Direction;
import net.minecraft.nbt.CompoundTag;
import net.minecraftforge.common.capabilities.Capability;
import net.minecraftforge.common.capabilities.ICapabilitySerializable;
import net.minecraftforge.common.util.LazyOptional;

import javax.annotation.Nonnull;
import javax.annotation.Nullable;

/**
 * Провайдер capability {@link PlayerMemory} для сущности игрока.
 *
 * <p>Вешается на любого {@code Player} (в том числе на фейковых игроков модов),
 * потому что {@code Player} наследуется и клиентской, и серверной сущностью.</p>
 */
public class PlayerMemoryProvider implements ICapabilitySerializable<CompoundTag> {

    private final PlayerMemory memory = new PlayerMemory();
    private final LazyOptional<PlayerMemory> optional = LazyOptional.of(() -> memory);

    @Nonnull
    @Override
    public <T> LazyOptional<T> getCapability(@Nonnull Capability<T> cap, @Nullable Direction side) {
        if (cap == PlayerMemory.CAPABILITY) {
            return optional.cast();
        }
        return LazyOptional.empty();
    }

    @Override
    public CompoundTag serializeNBT() {
        return memory.serializeNBT();
    }

    @Override
    public void deserializeNBT(CompoundTag nbt) {
        memory.deserializeNBT(nbt);
    }
}
