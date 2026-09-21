package com.thedirector.client;

import com.mojang.blaze3d.shaders.FogShape;
import com.thedirector.Config;
import com.thedirector.TheDirector;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.sounds.SoundSource;
import net.minecraft.util.RandomSource;
import net.minecraftforge.api.distmarker.Dist;
import net.minecraftforge.client.event.ComputeFovModifierEvent;
import net.minecraftforge.client.event.RenderGuiEvent;
import net.minecraftforge.client.event.ViewportEvent;
import net.minecraftforge.client.event.sound.PlaySoundEvent;
import net.minecraftforge.event.TickEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;
import net.minecraftforge.registries.ForgeRegistries;

import java.util.List;

/**
 * Все клиентские эффекты мода: FOV, тряска камеры, виньетка, туман, подмена заголовка
 * окна, ложный краш и фейковые строки чата.
 *
 * <p>Класс помечен {@link Dist#CLIENT} и никогда не загружается на выделенном сервере.</p>
 *
 * <p>Эффекты не спрашивают разрешения: режиссёр присылает пакет — клиент подчиняется.
 * Единственное, что клиент решает сам, — сколько эффект должен длиться, если сервер
 * об этом не сообщил.</p>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID, value = Dist.CLIENT)
public final class ClientEffects {

    /** Количество полос, которыми рисуется виньетка (больше — мягче градиент). */
    private static final int VIGNETTE_BANDS = 48;

    private static float fovMultiplier = 1.0F;
    private static int fovTicks;

    private static float shakeIntensity;
    private static int shakeTicks;

    private static float vignette;
    private static float fogDistance = -1.0F;
    private static int vignetteTicks;

    private static String originalWindowTitle;
    private static String overrideTitle;
    private static int titleTicks;

    private static boolean crashActive;
    private static String crashHeader = "";
    private static List<String> crashLines = List.of();
    private static int crashTicks;

    private static boolean screenshotPending;

    private ClientEffects() {
    }

    // ------------------------------------------------------------- API пакетов

    /** Проиграть звук в конкретной точке мира (только для этого клиента). */
    public static void playPositionalSound(ResourceLocation id, double x, double y, double z, float volume, float pitch) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.level == null) {
            return;
        }
        SoundEvent sound = ForgeRegistries.SOUND_EVENTS.getValue(id);
        if (sound == null) {
            return;
        }
        minecraft.getSoundManager().play(new SimpleSoundInstance(sound, SoundSource.AMBIENT,
                volume, pitch, RandomSource.create(), x, y, z));
    }

    /** Проиграть звук прямо на игроке (без привязки к точке мира). */
    public static void playSoundAtSelf(ResourceLocation id, float volume, float pitch) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.player == null) {
            return;
        }
        SoundEvent sound = ForgeRegistries.SOUND_EVENTS.getValue(id);
        if (sound == null) {
            return;
        }
        minecraft.getSoundManager().play(new SimpleSoundInstance(sound, SoundSource.AMBIENT,
                volume, pitch, RandomSource.create(), minecraft.player.getX(), minecraft.player.getY(),
                minecraft.player.getZ()));
    }

    /** Тряска камеры. */
    public static void shake(float intensity, int durationTicks) {
        shakeIntensity = Math.max(shakeIntensity, intensity);
        shakeTicks = Math.max(shakeTicks, durationTicks);
    }

    /** Сужение/расширение FOV. */
    public static void setFov(float multiplier, int durationTicks) {
        fovMultiplier = multiplier;
        fovTicks = Math.max(20, durationTicks);
    }

    /** Подмена заголовка окна. Пустая строка — вернуть исходный. */
    public static void setWindowTitle(String title, int holdTicks) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.getWindow() == null) {
            return;
        }
        if (originalWindowTitle == null) {
            originalWindowTitle = minecraft.getWindow().getTitle();
        }
        if (title.isEmpty()) {
            minecraft.getWindow().setTitle(originalWindowTitle);
            overrideTitle = null;
            titleTicks = 0;
            return;
        }
        minecraft.getWindow().setTitle(title);
        overrideTitle = title;
        titleTicks = Math.max(20, holdTicks);
    }

    /** Виньетка + туман. {@code fogDistance < 0} — туман не трогать. */
    public static void setVignette(float intensity, float distance, int durationTicks) {
        vignette = Math.max(vignette, intensity);
        vignetteTicks = Math.max(vignetteTicks, durationTicks);
        if (distance > 0.0F) {
            fogDistance = distance;
        }
    }

    /** Добавить строку в чат (используется для фейковых сообщений). */
    public static void addChatLine(Component text) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.gui == null) {
            return;
        }
        minecraft.gui.getChat().addMessage(text);
    }

    /** Запустить ложный краш. */
    public static void fakeCrash(String header, List<String> lines, int durationTicks, boolean takeScreenshot) {
        crashActive = true;
        crashHeader = header;
        crashLines = lines;
        crashTicks = Math.max(40, durationTicks);
        if (takeScreenshot) {
            screenshotPending = true;
        }
        ClientMetaHandler.note("FAKE_CRASH показан игроку (" + durationTicks + " тиков)");
    }

    /** Завершить ложный краш. */
    public static void endFakeCrash() {
        crashActive = false;
        crashTicks = 0;
        ClientMetaHandler.note("FAKE_CRASH завершён — мир вернулся");
    }

    /** Активен ли ложный краш (используется атмосферой, чтобы не мешать). */
    public static boolean isCrashActive() {
        return crashActive;
    }

    // ------------------------------------------------------------------- тики

    @SubscribeEvent
    public static void onClientTick(TickEvent.ClientTickEvent event) {
        if (event.phase != TickEvent.Phase.END) {
            return;
        }
        Minecraft minecraft = Minecraft.getInstance();

        if (fovTicks > 0 && --fovTicks == 0) {
            fovMultiplier = 1.0F;
        }
        if (shakeTicks > 0) {
            shakeTicks--;
            shakeIntensity *= 0.94F;
        } else {
            shakeIntensity = 0.0F;
        }
        if (vignetteTicks > 0) {
            vignetteTicks--;
        } else {
            vignette *= 0.97F;
            fogDistance = -1.0F;
        }
        if (titleTicks > 0 && --titleTicks == 0 && originalWindowTitle != null) {
            minecraft.getWindow().setTitle(originalWindowTitle);
            overrideTitle = null;
        }
        if (crashActive && --crashTicks <= 0) {
            // Страховка: если пакет о завершении потерялся, экран краха уходит сам
            endFakeCrash();
        }

        AtmosphereManager.tick(minecraft);

        if (screenshotPending) {
            screenshotPending = false;
            if (Config.metaScreenshots() && minecraft.gameDirectory != null) {
                Screenshot.grab(minecraft.gameDirectory, minecraft.getMainRenderTarget(),
                        component -> ClientMetaHandler.note("SCREENSHOT: " + component.getString()));
            }
        }
    }

    // ------------------------------------------------------------------ рендер

    /** Сужение FOV. */
    @SubscribeEvent
    public static void onComputeFov(ComputeFovModifierEvent event) {
        if (fovMultiplier != 1.0F) {
            event.setNewFovModifier(event.getNewFovModifier() * fovMultiplier);
        }
    }

    /** Тряска камеры. */
    @SubscribeEvent
    public static void onComputeCameraAngles(ViewportEvent.ComputeCameraAngles event) {
        float total = shakeIntensity + AtmosphereManager.getLocalShake();
        if (total <= 0.001F) {
            return;
        }
        double time = (System.nanoTime() % 1_000_000_000L) / 1.0E9D;
        event.setYaw(event.getYaw() + (float) (Math.sin(time * 13.3D) * total));
        event.setPitch(event.getPitch() + (float) (Math.cos(time * 17.7D) * total));
        event.setRoll(event.getRoll() + (float) (Math.sin(time * 9.1D) * total * 1.4F));
    }

    /** Туман без причины: он не зависит от биома и погоды. */
    @SubscribeEvent
    public static void onRenderFog(ViewportEvent.RenderFog event) {
        float distance = fogDistance > 0.0F ? fogDistance : AtmosphereManager.getLocalFogDistance();
        if (distance <= 0.0F) {
            return;
        }
        float far = Math.min(event.getFarPlaneDistance(), distance);
        float near = Math.min(event.getNearPlaneDistance(), Math.max(2.0F, distance * 0.35F));
        event.setFarPlaneDistance(far);
        event.setNearPlaneDistance(Math.min(near, far));
        event.setFogShape(FogShape.SPHERE);
    }

    /** Виньетка и экран ложного краха. */
    @SubscribeEvent
    public static void onRenderGui(RenderGuiEvent.Post event) {
        Minecraft minecraft = Minecraft.getInstance();
        GuiGraphics graphics = event.getGuiGraphics();
        int width = minecraft.getWindow().getGuiScaledWidth();
        int height = minecraft.getWindow().getGuiScaledHeight();

        float intensity = Math.max(vignette, AtmosphereManager.getLocalVignette());
        if (intensity > 0.01F && !crashActive) {
            drawVignette(graphics, width, height, intensity);
        }

        if (crashActive) {
            drawFakeCrash(minecraft, graphics, width, height);
        }
    }

    /** Виньетка строится горизонтальными полосами: альфа растёт к краям. */
    private static void drawVignette(GuiGraphics graphics, int width, int height, float intensity) {
        int bandHeight = Math.max(1, height / (VIGNETTE_BANDS * 2));
        for (int i = 0; i < VIGNETTE_BANDS; i++) {
            float t = i / (float) VIGNETTE_BANDS;
            int alpha = (int) (Math.pow(t, 2.4D) * intensity * 210.0F);
            if (alpha <= 1) {
                continue;
            }
            int color = (Math.min(alpha, 255) << 24);
            int offset = i * bandHeight;
            // сверху и снизу
            graphics.fill(0, offset, width, offset + bandHeight, color);
            graphics.fill(0, height - offset - bandHeight, width, height - offset, color);
        }
        // по бокам — мягче
        int sideWidth = Math.max(1, width / (VIGNETTE_BANDS * 2));
        for (int i = 0; i < VIGNETTE_BANDS / 2; i++) {
            float t = i / (float) (VIGNETTE_BANDS / 2);
            int alpha = (int) (Math.pow(t, 2.6D) * intensity * 150.0F);
            if (alpha <= 1) {
                continue;
            }
            int color = (Math.min(alpha, 255) << 24);
            int offset = i * sideWidth;
            graphics.fill(offset, 0, offset + sideWidth, height, color);
            graphics.fill(width - offset - sideWidth, 0, width - offset, height, color);
        }
    }

    /** Экран "краша": тёмный фон, заголовок, стек вызовов. */
    private static void drawFakeCrash(Minecraft minecraft, GuiGraphics graphics, int width, int height) {
        graphics.fill(0, 0, width, height, 0xF00C0A0A);
        graphics.fill(0, 0, width, 16, 0xFF3A1414);
        graphics.drawString(minecraft.font, crashHeader, 8, 4, 0xFFFF6E6E, false);

        int y = 26;
        for (String line : crashLines) {
            graphics.drawString(minecraft.font, line, 8, y, 0xFFB0B0B0, false);
            y += 10;
            if (y > height - 24) {
                break;
            }
        }
        String hint = "Minecraft has run out of memory.";
        graphics.drawString(minecraft.font, hint, 8, height - 30, 0xFF7A5A5A, false);
        graphics.drawString(minecraft.font, "Exit code: " + (128 + (System.nanoTime() & 0x1F)), 8, height - 20, 0xFF6A4A4A, false);
    }

    // ------------------------------------------------------------------ звук

    /** Приглушение музыки: часть "тишины", которая на самом деле не тишина. */
    @SubscribeEvent
    public static void onPlaySound(PlaySoundEvent event) {
        if (event.getSound() == null) {
            return;
        }
        if (AtmosphereManager.isMusicMuted() && event.getSound().getSource() == SoundSource.MUSIC) {
            event.setSound(null);
        }
    }

    /** Текущий наложенный заголовок окна (для отладки/мета-слоя). */
    public static String currentOverrideTitle() {
        return overrideTitle;
    }
}
