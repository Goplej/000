package com.thedirector.memory;

import net.minecraft.core.BlockPos;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.Tag;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;
import net.minecraftforge.common.capabilities.Capability;
import net.minecraftforge.common.capabilities.CapabilityManager;
import net.minecraftforge.common.capabilities.CapabilityToken;
import net.minecraftforge.common.util.INBTSerializable;

import java.util.ArrayList;
import java.util.List;

/**
 * Память игрока — единственный источник данных для режиссёра.
 *
 * <p>Хранится как capability на сущности игрока и сериализуется в NBT вместе с ним,
 * поэтому переживает перезаход в мир и (частично) смерть.</p>
 *
 * <p>Ключевые величины:</p>
 * <ul>
 *   <li>{@code safetyLevel} — насколько игрок расслаблен (0..20). Растёт в тишине.</li>
 *   <li>{@code dreadLevel} — насколько игрок напуган (0..20). Растёт от событий.</li>
 *   <li>{@code currentAct} — текущий акт драматургии (1..4).</li>
 *   <li>{@code baseLocation} — "дом" игрока, вычисляется по ночёвкам.</li>
 * </ul>
 */
public class PlayerMemory implements INBTSerializable<CompoundTag> {

    /** Максимальное значение шкал безопасности/страха. */
    public static final int MAX_LEVEL = 20;

    public static final Capability<PlayerMemory> CAPABILITY = CapabilityManager.get(new CapabilityToken<>() {
    });

    private static final int LOST_ITEMS_LIMIT = 27;
    private static final int HISTORY_LIMIT = 80;
    private static final int SNAPSHOT_LIMIT = 36;

    // ------------------------------------------------------------------ поля

    /** Насколько игрок чувствует себя в безопасности (0..20). */
    private int safetyLevel;
    /** Насколько игрок напуган (0..20). */
    private int dreadLevel;
    /** Текущий акт (1..4). */
    private int currentAct = 1;
    /** Количество дней в мире. */
    private long daysInWorld = 1L;
    /** Сколько тиков игрок провёл под землёй (y &lt; 56). */
    private long undergroundTime;
    /** Сколько источников света игрок поставил (факелы, фонари, костры). */
    private int lightUsage;
    /** Сколько раз игрок "встречался" с режиссёром (крупные события). */
    private int encounterCount;
    /** Последняя позиция, где игрока видели. */
    private BlockPos lastSeenPos;
    /** Дом игрока: вычисляется по статистике ночёвок. */
    private BlockPos baseLocation;
    /** Предметы, которые игрок выбросил/потерял. */
    private final List<ItemStack> lostItems = new ArrayList<>();
    /** История событий (id событий, максимум 80 записей). */
    private final List<String> eventsHistory = new ArrayList<>();
    /** Снимок инвентаря — нужен, чтобы отдавать игроку его же вещи в странных местах. */
    private final List<ItemStack> inventorySnapshot = new ArrayList<>();
    /** Сколько тиков игрок находится во сне (сериализуется, чтобы сон нельзя было прервать выходом). */
    private long dreamTicks;
    /** Сколько раз игрок попадал в измерение сна. */
    private int dreamVisits;
    /** Куда вернуть игрока после сна (точка, где он лёг). */
    private BlockPos returnAnchor;

    // ------------------------------------------------------- рантайм-состояние

    private boolean sleeping;
    private long lastEventTick;
    private long nextEventDelay;
    private String lastEventId = "";
    private int actStrikes;
    private BlockPos dreamAnchor;
    private int baseVisits;
    private long lastDreamAttemptDay = -1L;

    public PlayerMemory() {
    }

    /** Получить память игрока. Если capability недоступна — вернуть пустую (безопасный фолбэк). */
    public static PlayerMemory of(Player player) {
        return player.getCapability(CAPABILITY).orElseGet(PlayerMemory::new);
    }

    // --------------------------------------------------------------- геймплей

    /** Пассивный учёт: день в мире, время под землёй, время во сне. */
    public void tickPassive(ServerPlayer player) {
        Level level = player.level();
        long day = level.getDayTime() / 24000L + 1L;
        if (day > daysInWorld) {
            daysInWorld = day;
        }
        if (level.dimension().equals(Level.OVERWORLD) && player.getY() < 56.0D) {
            undergroundTime++;
        }
        if (com.thedirector.dream.DreamDimension.isDream(level)) {
            dreamTicks++;
        }
    }

    /** Прибавить безопасность (тишина). */
    public void addSafety(int amount) {
        safetyLevel = clamp(safetyLevel + amount);
    }

    /** Прибавить страх. Страх всегда немного съедает ощущение безопасности. */
    public void addDread(int amount) {
        dreadLevel = clamp(dreadLevel + amount);
        if (amount > 0) {
            safetyLevel = clamp(safetyLevel - Math.max(1, amount / 2));
        }
    }

    /** Записать событие в историю. */
    public void pushHistory(String eventId) {
        eventsHistory.add(eventId);
        while (eventsHistory.size() > HISTORY_LIMIT) {
            eventsHistory.remove(0);
        }
        lastEventId = eventId;
    }

    /** Игрок уже видел это событие? */
    public boolean hasSeen(String eventId) {
        return eventsHistory.contains(eventId);
    }

    /** Запомнить потерянную вещь. */
    public void rememberLostItem(ItemStack stack) {
        if (stack.isEmpty()) {
            return;
        }
        lostItems.add(stack.copy());
        while (lostItems.size() > LOST_ITEMS_LIMIT) {
            lostItems.remove(0);
        }
    }

    /** Забрать одну потерянную вещь (или пустой стак). */
    public ItemStack pollLostItem() {
        if (lostItems.isEmpty()) {
            return ItemStack.EMPTY;
        }
        return lostItems.remove(0);
    }

    /** Есть ли что возвращать. */
    public boolean hasLostItems() {
        return !lostItems.isEmpty();
    }

    /** Сохранить снимок инвентаря (для подмены вещей на "почти такие же"). */
    public void snapshotInventory(Player player) {
        inventorySnapshot.clear();
        for (int slot = 0; slot < Math.min(player.getInventory().getContainerSize(), SNAPSHOT_LIMIT); slot++) {
            ItemStack stack = player.getInventory().getItem(slot);
            if (!stack.isEmpty()) {
                inventorySnapshot.add(stack.copyWithCount(1));
            }
        }
    }

    /** Взять случайную вещь из снимка инвентаря. */
    public ItemStack randomSnapshotItem(java.util.Random random) {
        if (inventorySnapshot.isEmpty()) {
            return ItemStack.EMPTY;
        }
        return inventorySnapshot.get(random.nextInt(inventorySnapshot.size())).copy();
    }

    /** Сбросить состояние сна во время самого сна (точка возврата сохраняется). */
    public void clearDream() {
        dreamTicks = 0L;
        dreamAnchor = null;
    }

    /** Куда вернуть игрока после сна. */
    public BlockPos getReturnAnchor() {
        return returnAnchor;
    }

    public void setReturnAnchor(BlockPos pos) {
        this.returnAnchor = pos == null ? null : pos.immutable();
    }

    /** Скопировать память при смерти/возрождении. */
    public void copyFrom(PlayerMemory other) {
        this.safetyLevel = other.safetyLevel;
        this.dreadLevel = other.dreadLevel;
        this.currentAct = other.currentAct;
        this.daysInWorld = other.daysInWorld;
        this.undergroundTime = other.undergroundTime;
        this.lightUsage = other.lightUsage;
        this.encounterCount = other.encounterCount;
        this.lastSeenPos = other.lastSeenPos;
        this.baseLocation = other.baseLocation;
        this.dreamTicks = other.dreamTicks;
        this.dreamVisits = other.dreamVisits;
        this.baseVisits = other.baseVisits;
        this.returnAnchor = other.returnAnchor;
        this.lostItems.clear();
        other.lostItems.forEach(stack -> this.lostItems.add(stack.copy()));
        this.eventsHistory.clear();
        this.eventsHistory.addAll(other.eventsHistory);
        this.inventorySnapshot.clear();
        // Снимок инвентаря после смерти обнуляем: вещей у игрока больше нет
        this.lastEventId = other.lastEventId;
    }

    private static int clamp(int value) {
        return Math.max(0, Math.min(MAX_LEVEL, value));
    }

    // --------------------------------------------------------------- NBT

    @Override
    public CompoundTag serializeNBT() {
        CompoundTag tag = new CompoundTag();
        tag.putInt("safetyLevel", safetyLevel);
        tag.putInt("dreadLevel", dreadLevel);
        tag.putInt("currentAct", currentAct);
        tag.putLong("daysInWorld", daysInWorld);
        tag.putLong("undergroundTime", undergroundTime);
        tag.putInt("lightUsage", lightUsage);
        tag.putInt("encounterCount", encounterCount);
        if (lastSeenPos != null) {
            tag.putLong("lastSeenPos", lastSeenPos.asLong());
        }
        if (baseLocation != null) {
            tag.putLong("baseLocation", baseLocation.asLong());
        }
        if (returnAnchor != null) {
            tag.putLong("returnAnchor", returnAnchor.asLong());
        }
        tag.putLong("dreamTicks", dreamTicks);
        tag.putInt("dreamVisits", dreamVisits);
        tag.putInt("baseVisits", baseVisits);

        ListTag lost = new ListTag();
        for (ItemStack stack : lostItems) {
            lost.add(stack.save(new CompoundTag()));
        }
        tag.put("lostItems", lost);

        ListTag history = new ListTag();
        for (String id : eventsHistory) {
            history.add(net.minecraft.nbt.StringTag.valueOf(id));
        }
        tag.put("eventsHistory", history);

        ListTag snapshot = new ListTag();
        for (ItemStack stack : inventorySnapshot) {
            snapshot.add(stack.save(new CompoundTag()));
        }
        tag.put("inventorySnapshot", snapshot);
        return tag;
    }

    @Override
    public void deserializeNBT(CompoundTag tag) {
        safetyLevel = clamp(tag.getInt("safetyLevel"));
        dreadLevel = clamp(tag.getInt("dreadLevel"));
        currentAct = Math.max(1, Math.min(4, tag.getInt("currentAct")));
        daysInWorld = Math.max(1L, tag.getLong("daysInWorld"));
        undergroundTime = tag.getLong("undergroundTime");
        lightUsage = tag.getInt("lightUsage");
        encounterCount = tag.getInt("encounterCount");
        if (tag.contains("lastSeenPos")) {
            lastSeenPos = BlockPos.of(tag.getLong("lastSeenPos"));
        }
        if (tag.contains("baseLocation")) {
            baseLocation = BlockPos.of(tag.getLong("baseLocation"));
        }
        if (tag.contains("returnAnchor")) {
            returnAnchor = BlockPos.of(tag.getLong("returnAnchor"));
        }
        dreamTicks = tag.getLong("dreamTicks");
        dreamVisits = tag.getInt("dreamVisits");
        baseVisits = tag.getInt("baseVisits");

        lostItems.clear();
        ListTag lost = tag.getList("lostItems", Tag.TAG_COMPOUND);
        for (int i = 0; i < lost.size(); i++) {
            lostItems.add(ItemStack.of(lost.getCompound(i)));
        }

        eventsHistory.clear();
        ListTag history = tag.getList("eventsHistory", Tag.TAG_STRING);
        for (int i = 0; i < history.size(); i++) {
            eventsHistory.add(history.getString(i));
        }

        inventorySnapshot.clear();
        ListTag snapshot = tag.getList("inventorySnapshot", Tag.TAG_COMPOUND);
        for (int i = 0; i < snapshot.size(); i++) {
            inventorySnapshot.add(ItemStack.of(snapshot.getCompound(i)));
        }
    }

    // --------------------------------------------------------------- геттеры

    public int getSafetyLevel() {
        return safetyLevel;
    }

    public void setSafetyLevel(int value) {
        this.safetyLevel = clamp(value);
    }

    public int getDreadLevel() {
        return dreadLevel;
    }

    public void setDreadLevel(int value) {
        this.dreadLevel = clamp(value);
    }

    public int getCurrentAct() {
        return currentAct;
    }

    public void setCurrentAct(int act) {
        this.currentAct = Math.max(1, Math.min(4, act));
    }

    public long getDaysInWorld() {
        return daysInWorld;
    }

    public void setDaysInWorld(long days) {
        this.daysInWorld = Math.max(1L, days);
    }

    public long getUndergroundTime() {
        return undergroundTime;
    }

    public int getLightUsage() {
        return lightUsage;
    }

    public void addLightUsage(int amount) {
        this.lightUsage = Math.max(0, this.lightUsage + amount);
    }

    public int getEncounterCount() {
        return encounterCount;
    }

    public void setEncounterCount(int value) {
        this.encounterCount = Math.max(0, value);
    }

    public BlockPos getLastSeenPos() {
        return lastSeenPos;
    }

    public void setLastSeenPos(BlockPos pos) {
        this.lastSeenPos = pos == null ? null : pos.immutable();
    }

    public BlockPos getBaseLocation() {
        return baseLocation;
    }

    public void setBaseLocation(BlockPos pos) {
        this.baseLocation = pos == null ? null : pos.immutable();
        this.baseVisits = 0;
    }

    public boolean hasBase() {
        return baseLocation != null;
    }

    public int getBaseVisits() {
        return baseVisits;
    }

    public void setBaseVisits(int visits) {
        this.baseVisits = visits;
    }

    public int getDreamVisits() {
        return dreamVisits;
    }

    public void setDreamVisits(int visits) {
        this.dreamVisits = visits;
    }

    public long getDreamTicks() {
        return dreamTicks;
    }

    public boolean isSleeping() {
        return sleeping;
    }

    public void setSleeping(boolean value) {
        this.sleeping = value;
    }

    public long getLastEventTick() {
        return lastEventTick;
    }

    public void setLastEventTick(long tick) {
        this.lastEventTick = tick;
    }

    public long getNextEventDelay() {
        return nextEventDelay;
    }

    public void setNextEventDelay(long delay) {
        this.nextEventDelay = delay;
    }

    public String getLastEventId() {
        return lastEventId;
    }

    public int getActStrikes() {
        return actStrikes;
    }

    public void setActStrikes(int value) {
        this.actStrikes = value;
    }

    public BlockPos getDreamAnchor() {
        return dreamAnchor;
    }

    public void setDreamAnchor(BlockPos pos) {
        this.dreamAnchor = pos == null ? null : pos.immutable();
    }

    public long getLastDreamAttemptDay() {
        return lastDreamAttemptDay;
    }

    public void setLastDreamAttemptDay(long day) {
        this.lastDreamAttemptDay = day;
    }

    public List<String> getEventsHistory() {
        return eventsHistory;
    }

    public List<ItemStack> getLostItems() {
        return lostItems;
    }

    public List<ItemStack> getInventorySnapshot() {
        return inventorySnapshot;
    }
}
