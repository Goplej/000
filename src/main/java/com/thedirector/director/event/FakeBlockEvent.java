package com.thedirector.director.event;

import com.thedirector.director.Act;
import com.thedirector.director.DirectorEvent;
import com.thedirector.memory.PlayerMemory;
import com.thedirector.util.ScheduledTasks;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

/**
 * Блок, которого не должно быть.
 *
 * <p>Самый простой микро-сдвиг реальности: рядом появляется один блок, идеально
 * вписанный в окружение (камень, гравий, земля, доски). В акте 1-2 он исчезает через
 * полминуты — игрок успевает только "почти заметить". В акте 4 остаётся навсегда.</p>
 */
public class FakeBlockEvent extends DirectorEvent {

    public FakeBlockEvent() {
        super("fake_block", Act.ONE, 16, 1, 2, 20 * 60 * 5);
    }

    @Override
    public void execute(Player player, Level level, PlayerMemory memory) {
        Act act = Act.of(memory.getCurrentAct());
        BlockPos anchor = findSpot(level, player.blockPosition());
        if (anchor == null) {
            return;
        }
        Block block = chooseBlock(level, act);
        BlockState state = block.defaultBlockState();

        level.setBlock(anchor, state, 3);

        // В акте 4 блок остаётся: мир уже не вернуть.
        if (act.number() <= 3) {
            int life = 400 + level.random.nextInt(1200);
            ScheduledTasks.schedule(level, life, () -> {
                if (level.getBlockState(anchor) == state) {
                    level.setBlock(anchor, Blocks.AIR.defaultBlockState(), 3);
                }
            });
        }
    }

    private Block chooseBlock(Level level, Act act) {
        return switch (act) {
            case ONE -> level.random.nextBoolean() ? Blocks.GRAVEL : Blocks.COBBLESTONE;
            case TWO -> switch (level.random.nextInt(4)) {
                case 0 -> Blocks.GRAVEL;
                case 1 -> Blocks.COBBLESTONE;
                case 2 -> Blocks.DIRT;
                default -> Blocks.STONE;
            };
            case THREE -> switch (level.random.nextInt(4)) {
                case 0 -> Blocks.BLACKSTONE;
                case 1 -> Blocks.DEEPSLATE;
                case 2 -> Blocks.COBBLESTONE;
                default -> Blocks.OAK_PLANKS;
            };
            case FOUR -> switch (level.random.nextInt(4)) {
                case 0 -> Blocks.BLACKSTONE;
                case 1 -> Blocks.SOUL_SAND;
                case 2 -> Blocks.DEEPSLATE_TILES;
                default -> Blocks.BASALT;
            };
        };
    }

    /** Ищем точку на земле рядом с игроком, куда блок встанет "сам собой". */
    private BlockPos findSpot(Level level, BlockPos center) {
        for (int attempt = 0; attempt < 40; attempt++) {
            BlockPos candidate = center.offset(level.random.nextInt(11) - 5,
                    level.random.nextInt(5) - 2,
                    level.random.nextInt(11) - 5);
            if (candidate.distSqr(center) < 4.0D) {
                continue;
            }
            BlockPos below = candidate.below();
            if (level.isEmptyBlock(candidate) && !level.isEmptyBlock(below)
                    && level.getBlockState(below).isSolidRender(level, below)) {
                return candidate.immutable();
            }
        }
        return null;
    }
}
