package com.thedirector.dream;

import com.thedirector.TheDirector;
import com.thedirector.memory.PlayerMemory;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.Heightmap;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Строитель сна.
 *
 * <p>Копирует участок мира вокруг базы игрока (100x100 блоков) в измерение сна.
 * Копирование разбито на порции по {@value #BLOCKS_PER_TICK} блоков за тик, чтобы не
 * ронять TPS, и завершается до появления игрока в измерении.</p>
 *
 * <p>Отдельно решается задача "ломается, но возвращается": для каждого поставленного
 * блока запоминается его состояние, поэтому разбитый блок можно восстановить через
 * секунду (см. {@link DreamEvents}).</p>
 */
public final class DreamReplicator {

    /** Сколько блоков копируется за один тик. */
    private static final int BLOCKS_PER_TICK = 24_000;

    /** Радиус копирования: 50 блоков в каждую сторону = 100x100. */
    public static final int RADIUS = 50;

    /** На сколько блоков ниже поверхности копируем. */
    private static final int DEPTH_BELOW = 4;

    /** На сколько блоков выше поверхности копируем (дома, деревья, крыши). */
    private static final int HEIGHT_ABOVE = 14;

    /** Состояния блоков последнего сна: позиция -> состояние (для восстановления). */
    private static final Map<BlockPos, BlockState> LAST_DREAM = new HashMap<>();

    /** Очередь незавершённого копирования. */
    private static final Deque<Column> PENDING = new ArrayDeque<>();

    /** Точка, где игрок окажется после копирования (центр участка, поверхность). */
    private static BlockPos spawnPoint;

    /** Задача, которую нужно выполнить после завершения копирования. */
    private static Runnable onFinished;

    private record Column(int x, int z, int fromY, int toY, ServerLevel source) {
    }

    private DreamReplicator() {
    }

    /** Сон строится прямо сейчас? */
    public static boolean isBuilding() {
        return !PENDING.isEmpty();
    }

    /**
     * Начать построение сна.
     *
     * @param dream  измерение сна
     * @param source мир, откуда пришёл игрок (копируем именно его)
     * @param center центр копирования (обычно база игрока)
     * @param player игрок, который видит сон
     * @param after  что выполнить после завершения (обычно — телепорт и порча предметов)
     */
    public static void begin(ServerLevel dream, ServerLevel source, BlockPos center, ServerPlayer player, Runnable after) {
        // Сначала убираем прошлый сон (иначе измерение будет накапливать копии)
        clear(dream);
        PENDING.clear();
        onFinished = after;
        spawnPoint = null;

        for (int dx = -RADIUS; dx <= RADIUS; dx++) {
            for (int dz = -RADIUS; dz <= RADIUS; dz++) {
                int x = center.getX() + dx;
                int z = center.getZ() + dz;
                int surface = source.getHeight(Heightmap.Types.MOTION_BLOCKING_NO_LEAVES, x, z);
                // Если в исходном мире тут пустота — берём уровень ног игрока
                if (surface <= source.getMinBuildHeight()) {
                    surface = center.getY();
                }
                PENDING.add(new Column(x, z, surface - DEPTH_BELOW, surface + HEIGHT_ABOVE, source));
            }
        }
        TheDirector.LOGGER.info("[The Director] Сон строится: {} колонн, центр {}", PENDING.size(), center);
    }

    /** Продолжить копирование (вызывается каждый тик для измерения сна). */
    public static void tick(ServerLevel dream) {
        if (PENDING.isEmpty()) {
            return;
        }
        int budget = BLOCKS_PER_TICK;
        while (budget > 0 && !PENDING.isEmpty()) {
            Column column = PENDING.poll();
            budget -= copyColumn(dream, column);
        }
        if (PENDING.isEmpty()) {
            finish(dream);
        }
    }

    /** Копирование одной колонны: снизу вверх, только воздух в приёмнике. */
    private static int copyColumn(ServerLevel dream, Column column) {
        int copied = 0;
        for (int y = column.fromY(); y <= column.toY(); y++) {
            BlockPos from = new BlockPos(column.x(), y, column.z());
            BlockState state = column.source().getBlockState(from);
            if (state.isAir()) {
                continue;
            }
            BlockPos to = new BlockPos(column.x(), y, column.z());
            if (!dream.getBlockState(to).isAir()) {
                continue;
            }
            dream.setBlock(to, state, 2);
            LAST_DREAM.put(to.immutable(), state);
            copied++;
        }
        return copied;
    }

    /** Копирование завершено: отмечаем точку выхода и запускаем продолжение. */
    private static void finish(ServerLevel dream) {
        if (spawnPoint == null) {
            // Находим центр участка и ставим игрока на верхний блок
            BlockPos any = LAST_DREAM.keySet().stream().findFirst().orElse(dream.getSharedSpawnPos());
            spawnPoint = any.above(2);
        }
        TheDirector.LOGGER.info("[The Director] Сон готов: {} блоков, точка входа {}", LAST_DREAM.size(), spawnPoint);
        Runnable callback = onFinished;
        onFinished = null;
        if (callback != null) {
            callback.run();
        }
    }

    /** Точка входа в сон (после завершения копирования). */
    public static BlockPos spawnPoint() {
        return spawnPoint;
    }

    /** Состояние блока из сна — используется для восстановления разбитого блока. */
    public static BlockState originalState(BlockPos pos) {
        return LAST_DREAM.get(pos);
    }

    /**
     * Вернуть блок на место. Возвращает {@code true}, если блок был частью сна
     * и его удалось восстановить.
     */
    public static boolean restore(ServerLevel dream, BlockPos pos) {
        BlockState original = LAST_DREAM.get(pos);
        if (original == null) {
            return false;
        }
        dream.setBlock(pos, original, 3);
        return true;
    }

    /** Пометить блок как часть сна (например, поставленный игроком внутри сна). */
    public static void mark(BlockPos pos, BlockState state) {
        LAST_DREAM.put(pos.immutable(), state);
    }

    /**
     * Полностью убрать построенный сон: измерение не должно накапливать мусор
     * между посещениями.
     */
    public static void clear(ServerLevel dream) {
        for (Map.Entry<BlockPos, BlockState> entry : LAST_DREAM.entrySet()) {
            BlockPos pos = entry.getKey();
            dream.setBlock(pos, Blocks.AIR.defaultBlockState(), 2);
        }
        LAST_DREAM.clear();
    }

    /** Сброс очереди (вызывается при старте нового сна и остановке сервера). */
    public static void clear() {
        PENDING.clear();
        LAST_DREAM.clear();
        spawnPoint = null;
        onFinished = null;
    }

    /** Найти безопасную точку над поверхностью в измерении сна. */
    public static BlockPos findSafeSpot(Level level, BlockPos hint) {
        BlockPos.MutableBlockPos mutable = hint.mutable();
        for (int y = Math.min(level.getMaxBuildHeight() - 2, hint.getY() + 40); y > level.getMinBuildHeight(); y--) {
            mutable.setY(y);
            if (!level.isEmptyBlock(mutable) && level.isEmptyBlock(mutable.above())
                    && level.isEmptyBlock(mutable.above(2))) {
                return mutable.above().immutable();
            }
        }
        return hint.above(2);
    }

    /** Копирование "части сна" вокруг точки (используется, если игрок ушёл далеко). */
    public static List<BlockPos> borderOf(BlockPos center, int radius) {
        List<BlockPos> result = new ArrayList<>();
        for (Direction direction : Direction.Plane.HORIZONTAL) {
            BlockPos offset = center.relative(direction, radius);
            result.add(offset);
        }
        return result;
    }
}
