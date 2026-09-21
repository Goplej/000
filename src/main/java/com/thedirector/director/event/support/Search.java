package com.thedirector.director.event.support;

import net.minecraft.core.BlockPos;
import net.minecraft.world.Container;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BarrelBlockEntity;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.entity.ChestBlockEntity;
import net.minecraft.world.level.block.entity.ShulkerBoxBlockEntity;
import net.minecraft.world.level.block.state.BlockState;

import javax.annotation.Nullable;
import java.util.ArrayList;
import java.util.List;

/**
 * Поиск по миру: контейнеры, источники света, "странное место" для предмета.
 *
 * <p>Все методы работают только на сервере и стараются обходиться малым радиусом,
 * чтобы не нагружать TPS.</p>
 */
public final class Search {

    /** Блоки, которые мод считает источниками света (их он умеет гасить и возвращать). */
    private static final List<Block> LIGHT_BLOCKS = List.of(
            Blocks.TORCH, Blocks.WALL_TORCH, Blocks.SOUL_TORCH, Blocks.SOUL_WALL_TORCH,
            Blocks.LANTERN, Blocks.SOUL_LANTERN, Blocks.CAMPFIRE, Blocks.SOUL_CAMPFIRE,
            Blocks.GLOWSTONE, Blocks.SEA_LANTERN, Blocks.REDSTONE_LAMP, Blocks.END_ROD,
            Blocks.SHROOMLIGHT, Blocks.OCHRE_FROGLIGHT, Blocks.VERDANT_FROGLIGHT, Blocks.PEARLESCENT_FROGLIGHT);

    /** Все ли источники света этого блока поддерживаются модом. */
    public static boolean isLightSource(BlockState state) {
        return LIGHT_BLOCKS.contains(state.getBlock());
    }

    /** Найти ближайший контейнер в радиусе (сундук, бочка, шалкер). */
    @Nullable
    public static Container findContainer(Level level, BlockPos center, int radius) {
        Container best = null;
        double bestDistance = Double.MAX_VALUE;
        for (BlockPos pos : BlockPos.betweenClosed(center.offset(-radius, -3, -radius), center.offset(radius, 3, radius))) {
            BlockEntity blockEntity = level.getBlockEntity(pos);
            if (isPlayerContainer(blockEntity)) {
                double distance = pos.distSqr(center);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = (Container) blockEntity;
                }
            }
        }
        return best;
    }

    private static boolean isPlayerContainer(@Nullable BlockEntity blockEntity) {
        return blockEntity instanceof ChestBlockEntity
                || blockEntity instanceof BarrelBlockEntity
                || blockEntity instanceof ShulkerBoxBlockEntity;
    }

    /** Найти источники света вокруг центра. */
    public static List<BlockPos> findLights(Level level, BlockPos center, int radius, int limit) {
        List<BlockPos> found = new ArrayList<>();
        for (BlockPos pos : BlockPos.betweenClosed(center.offset(-radius, -6, -radius), center.offset(radius, 6, radius))) {
            if (isLightSource(level.getBlockState(pos))) {
                found.add(pos.immutable());
                if (found.size() >= limit) {
                    break;
                }
            }
        }
        return found;
    }

    /**
     * Позиция для "странного места": где угодно рядом, но так, чтобы игрок её не ожидал —
     * за спиной, на крыше, в стене.
     */
    @Nullable
    public static BlockPos strangeSpot(Level level, BlockPos center, int radius, java.util.Random random) {
        for (int attempt = 0; attempt < 24; attempt++) {
            BlockPos candidate = center.offset(random.nextInt(radius * 2 + 1) - radius,
                    random.nextInt(7) - 2,
                    random.nextInt(radius * 2 + 1) - radius);
            BlockState state = level.getBlockState(candidate);
            if (state.isAir() || state.canBeReplaced()) {
                BlockPos below = candidate.below();
                if (!level.getBlockState(below).isAir()) {
                    return candidate.immutable();
                }
            }
        }
        return null;
    }

    /** Есть ли над игроком крыша (используется для "шагов на крыше"). */
    public static boolean hasRoofAbove(Level level, BlockPos center, int maxHeight) {
        for (int y = 1; y <= maxHeight; y++) {
            BlockPos pos = center.above(y);
            BlockState state = level.getBlockState(pos);
            if (!state.isAir() && state.isSolidRender(level, pos)) {
                return true;
            }
        }
        return false;
    }

    /** Технический метод-заглушка не нужен, но проверка контекста — нужна. */
    public static boolean alwaysTrue() {
        return true;
    }

    private Search() {
    }
}
