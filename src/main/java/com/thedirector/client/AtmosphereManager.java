package com.thedirector.client;

import com.thedirector.Config;
import com.thedirector.ModSounds;
import com.thedirector.dream.DreamDimension;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.sounds.SoundSource;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

/**
 * Менеджер атмосферы (клиентская часть).
 *
 * <p>Задача — держать игрока в состоянии "что-то не так" без единого события:
 * мерцающие факелы, дыхание темноты, приглушённая музыка, локальная виньетка
 * и туман, который не зависит от биома и погоды.</p>
 *
 * <p>Атмосфера никогда не "бьёт" — она готовит. Удар приходит от событий режиссёра.</p>
 */
public final class AtmosphereManager {

    private static final RandomSource RANDOM = RandomSource.create();

    private static int flickerCooldown;
    private static int ambientCooldown;
    private static float localVignette;
    private static float localShake;
    private static float localFogDistance = -1.0F;
    private static boolean musicMuted;

    private AtmosphereManager() {
    }

    /** Вызывается каждый клиентский тик из {@link ClientEffects}. */
    public static void tick(Minecraft minecraft) {
        LocalPlayer player = minecraft.player;
        Level level = minecraft.level;
        if (player == null || level == null) {
            reset();
            return;
        }

        boolean inDream = DreamDimension.isDream(level);

        // --- Локальная тревога: ночь, темнота, подземелье ---
        float darkness = 1.0F - Math.max(0.05F, level.getMaxLocalRawBrightness(player.blockPosition()) / 15.0F);
        float targetVignette = 0.12F + darkness * 0.35F;
        if (inDream) {
            // Во сне темнота всегда рядом, даже когда светло
            targetVignette = 0.45F;
        }
        localVignette += (targetVignette - localVignette) * 0.02F;

        // --- Туман: чем темнее и чем глубже, тем ближе горизонт ---
        if (inDream) {
            localFogDistance = 42.0F;
        } else if (player.getY() < 40.0D) {
            localFogDistance = 96.0F;
        } else {
            localFogDistance = -1.0F;
        }

        // --- Мерцание факелов ---
        if (Config.torchFlicker() && flickerCooldown-- <= 0) {
            flickerCooldown = 6 + RANDOM.nextInt(14);
            flickerTorch(level, player);
        }

        // --- Фоновые звуки атмосферы ---
        if (ambientCooldown-- <= 0) {
            ambientCooldown = 200 + RANDOM.nextInt(400);
            playAmbient(player, level, inDream);
        }

        // --- Приглушение музыки: в темноте, во сне и под землёй ---
        musicMuted = inDream || darkness > 0.75F || player.getY() < 30.0D;

        // --- Во сне игрок слышит собственное эхо ---
        if (inDream && player.walkDist > 0.1F && player.tickCount % 22 == 0) {
            Vec3 behind = player.position().subtract(player.getLookAngle().scale(3.0D));
            minecraft.getSoundManager().play(new net.minecraft.client.resources.sounds.SimpleSoundInstance(
                    ModSounds.STEP_ECHO.get(), SoundSource.AMBIENT, 0.35F, 0.7F, RANDOM,
                    behind.x, behind.y, behind.z));
        }

        // Лёгкая дрожь при очень высокой темноте (эффект "здесь нечем дышать")
        localShake = darkness > 0.9F && !inDream ? 0.25F : 0.0F;
    }

    private static void flickerTorch(Level level, LocalPlayer player) {
        BlockPos center = player.blockPosition();
        for (int attempt = 0; attempt < 6; attempt++) {
            BlockPos pos = center.offset(RANDOM.nextInt(13) - 6, RANDOM.nextInt(7) - 3, RANDOM.nextInt(13) - 6);
            BlockState state = level.getBlockState(pos);
            if (!com.thedirector.director.event.support.Search.isLightSource(state)) {
                continue;
            }
            // Пламя "дышит": искра уходит вверх, а не горит ровно
            level.addParticle(ParticleTypes.SMOKE,
                    pos.getX() + 0.5D + (RANDOM.nextDouble() - 0.5D) * 0.3D,
                    pos.getY() + 0.75D,
                    pos.getZ() + 0.5D + (RANDOM.nextDouble() - 0.5D) * 0.3D,
                    0.0D, 0.01D, 0.0D);
            if (RANDOM.nextFloat() < 0.25F) {
                level.addParticle(ParticleTypes.FLAME,
                        pos.getX() + 0.5D, pos.getY() + 0.6D, pos.getZ() + 0.5D,
                        0.0D, 0.0D, 0.0D);
            }
            if (RANDOM.nextFloat() < 0.08F) {
                level.playLocalSound(pos.getX(), pos.getY(), pos.getZ(),
                        net.minecraft.sounds.SoundEvents.FIRE_AMBIENT, SoundSource.AMBIENT, 0.35F, 0.6F, false);
            }
            return;
        }
    }

    private static void playAmbient(LocalPlayer player, Level level, boolean inDream) {
        double x = player.getX() + (RANDOM.nextDouble() - 0.5D) * 12.0D;
        double y = player.getY() + RANDOM.nextDouble() * 4.0D - 1.0D;
        double z = player.getZ() + (RANDOM.nextDouble() - 0.5D) * 12.0D;

        if (inDream) {
            level.playLocalSound(x, y, z, ModSounds.VOID_AMBIENT.get(), SoundSource.AMBIENT, 0.4F, 0.5F, false);
        } else if (player.getY() < 40.0D && RANDOM.nextFloat() < 0.5F) {
            level.playLocalSound(x, y, z, ModSounds.WOOD_CREAK.get(), SoundSource.AMBIENT, 0.25F, 0.4F, false);
        } else if (RANDOM.nextFloat() < 0.2F) {
            // Далёкий шаг, который никому не принадлежит
            level.playLocalSound(x, y, z, ModSounds.ROOF_STEP.get(), SoundSource.AMBIENT, 0.2F, 0.5F, false);
        }
    }

    private static void reset() {
        localVignette = 0.0F;
        localShake = 0.0F;
        localFogDistance = -1.0F;
        musicMuted = false;
    }

    /** Локальная виньетка (от темноты и сна). */
    public static float getLocalVignette() {
        return localVignette;
    }

    /** Локальная дрожь (от темноты). */
    public static float getLocalShake() {
        return localShake;
    }

    /** Локальный туман; -1 = не вмешиваться. */
    public static float getLocalFogDistance() {
        return localFogDistance;
    }

    /** Нужно ли приглушить музыку прямо сейчас. */
    public static boolean isMusicMuted() {
        return musicMuted;
    }
}
