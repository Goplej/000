package com.thedirector.command;

import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.context.CommandContext;
import com.mojang.brigadier.exceptions.CommandSyntaxException;
import com.thedirector.TheDirector;
import com.thedirector.director.Act;
import com.thedirector.director.Director;
import com.thedirector.director.DirectorEvent;
import com.thedirector.director.EventDirector;
import com.thedirector.dream.DreamDimension;
import com.thedirector.memory.PlayerMemory;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraftforge.event.RegisterCommandsEvent;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;

import java.util.stream.Collectors;

/**
 * Диагностические команды {@code /thedirector}.
 *
 * <p>Хоррор по своей природе незаметен: игрок не должен понимать, случайность это или
 * режиссёр. Поэтому мод даёт отдельный инструмент проверки — для автора мода, владельца
 * сервера и для того, кто просто хочет убедиться, что мод действительно работает.</p>
 *
 * <p>Права: {@code status} и {@code events} доступны всем (только чтение). Всё, что меняет
 * состояние игры — {@code act}, {@code event}, {@code dream}, {@code wake} — требует уровня
 * оператора (2). В одиночной игре нужны включённые читы.</p>
 *
 * <p>Примеры:</p>
 * <pre>
 *   /thedirector status            — акт, страх, безопасность, дом, счётчики
 *   /thedirector events            — список 13 событий и доступно ли каждое сейчас
 *   /thedirector event whisper     — выполнить конкретное событие немедленно
 *   /thedirector act 3             — перейти в акт 3
 *   /thedirector dream             — принудительно попасть в измерение Сна
 *   /thedirector wake              — выйти из сна
 * </pre>
 */
@Mod.EventBusSubscriber(modid = TheDirector.MOD_ID)
public final class DirectorCommands {

    private DirectorCommands() {
    }

    /** Регистрация команд (без них диагностировать мод почти невозможно). */
    @SubscribeEvent
    public static void onRegisterCommands(RegisterCommandsEvent event) {
        CommandDispatcher<CommandSourceStack> dispatcher = event.getDispatcher();
        dispatcher.register(Commands.literal("thedirector")
                // чтение — всем
                .then(Commands.literal("status").executes(DirectorCommands::status))
                .then(Commands.literal("events").executes(DirectorCommands::listEvents))
                // изменение мира — только операторам
                .then(Commands.literal("act")
                        .requires(source -> source.hasPermission(2))
                        .then(Commands.argument("number", IntegerArgumentType.integer(1, 4))
                                .executes(DirectorCommands::setAct)))
                .then(Commands.literal("event")
                        .requires(source -> source.hasPermission(2))
                        .then(Commands.argument("id", StringArgumentType.word())
                                .suggests((context, builder) -> {
                                    EventDirector.all().forEach(e -> builder.suggest(e.id()));
                                    return builder.buildFuture();
                                })
                                .executes(DirectorCommands::runEvent)))
                .then(Commands.literal("dream")
                        .requires(source -> source.hasPermission(2))
                        .executes(DirectorCommands::enterDream))
                .then(Commands.literal("wake")
                        .requires(source -> source.hasPermission(2))
                        .executes(DirectorCommands::leaveDream)));
    }

    // ------------------------------------------------------------------ status

    /** Полное состояние режиссёра по конкретному игроку. */
    private static int status(CommandContext<CommandSourceStack> context) {
        CommandSourceStack source = context.getSource();
        ServerPlayer player = source.getEntity() instanceof ServerPlayer serverPlayer ? serverPlayer : null;
        if (player == null) {
            source.sendSystemMessage(Component.literal("[The Director] Состояние привязано к игроку — "
                    + "выполните команду от имени игрока. Событий в реестре: " + EventDirector.all().size()));
            return 1;
        }

        PlayerMemory memory = PlayerMemory.of(player);
        Act act = Act.of(memory.getCurrentAct());
        long now = player.serverLevel().getGameTime();
        long silence = memory.getLastEventTick() == 0L ? -1L : now - memory.getLastEventTick();

        source.sendSystemMessage(Component.literal("§8=== The Director ==="));
        line(source, "Акт", act.number() + " (" + act.label() + ")");
        line(source, "Дней в мире", String.valueOf(memory.getDaysInWorld()));
        line(source, "Безопасность / страх", memory.getSafetyLevel() + " / " + memory.getDreadLevel()
                + " §8(правило: safety>15 и dread<5 — можно бить; dread>15 — тишина)");
        line(source, "Измерение", DreamDimension.isDream(player.level()) ? "§7сон" : "обычный мир");
        line(source, "Дом (по ночёвкам)", memory.hasBase() ? memory.getBaseLocation().toShortString()
                : "ещё не определён (нужно 3 ночи в одном месте)");
        line(source, "Поставлено света", String.valueOf(memory.getLightUsage()));
        line(source, "Время под землёй", memory.getUndergroundTime() / 20L + " сек");
        line(source, "Событий сработало", memory.getEncounterCount() + " §8("
                + memory.getEventsHistory().size() + " записей в истории)");
        line(source, "Последнее событие", memory.getLastEventId().isEmpty() ? "—" : memory.getLastEventId());
        line(source, "Молчание", silence < 0 ? "с начала мира" : silence / 20L + " сек");
        line(source, "Пауза до следующего", memory.getNextEventDelay() / 20L + " сек");
        line(source, "Потерянных вещей в памяти", String.valueOf(memory.getLostItems().size()));
        line(source, "Посещений сна", String.valueOf(memory.getDreamVisits()));
        return 1;
    }

    // ------------------------------------------------------------------ events

    /** Список событий: что доступно прямо сейчас, а что ещё закрыто актом или контекстом. */
    private static int listEvents(CommandContext<CommandSourceStack> context) {
        CommandSourceStack source = context.getSource();
        ServerPlayer player = source.getEntity() instanceof ServerPlayer serverPlayer ? serverPlayer : null;
        PlayerMemory memory = player == null ? new PlayerMemory() : PlayerMemory.of(player);
        Act act = Act.of(memory.getCurrentAct());

        source.sendSystemMessage(Component.literal("§8=== События режиссёра (" + EventDirector.all().size()
                + "), текущий акт " + act.number() + " ==="));
        for (DirectorEvent event : EventDirector.all()) {
            boolean byAct = event.minAct().number() <= act.number();
            boolean byContext = player == null || event.canRun(player, memory, player.serverLevel());
            String mark = byAct && byContext ? "§a✔" : (byAct ? "§e~" : "§8✘");
            String reason = byAct && byContext ? ""
                    : (!byAct ? " §8(нужен акт " + event.minAct().number() + "+)" : " §8(нет подходящего места рядом)");
            source.sendSystemMessage(Component.literal(mark + " §f" + event.id()
                    + " §8вес " + event.weight()
                    + " §8страх +" + event.dreadCost() + reason));
        }
        source.sendSystemMessage(Component.literal("§8✔ доступно сейчас   ~ нужен контекст   ✘ закрыто актом"));
        return 1;
    }

    // -------------------------------------------------------------------- act

    /** Перескок в нужный акт — для проверки драматургии за один вечер. */
    private static int setAct(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        int number = IntegerArgumentType.getInteger(context, "number");
        PlayerMemory memory = PlayerMemory.of(player);
        Act act = Act.of(number);
        memory.setCurrentAct(act.number());
        memory.setDaysInWorld(act.scaledStartDay());
        context.getSource().sendSystemMessage(Component.literal("§7[The Director] Акт переключён: §f"
                + act.number() + " — " + act.label()));
        return 1;
    }

    // ------------------------------------------------------------------ event

    /** Немедленно выполнить конкретное событие — «он точно работает?». */
    private static int runEvent(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        String id = StringArgumentType.getString(context, "id");
        DirectorEvent found = EventDirector.all().stream()
                .filter(event -> event.id().equalsIgnoreCase(id))
                .findFirst()
                .orElse(null);
        if (found == null) {
            context.getSource().sendSystemMessage(Component.literal("§c[The Director] Нет события «" + id
                    + "». Доступные: " + EventDirector.all().stream()
                    .map(DirectorEvent::id).collect(Collectors.joining(", "))));
            return 0;
        }
        PlayerMemory memory = PlayerMemory.of(player);
        Director.fireEventNow(player, memory, found);
        context.getSource().sendSystemMessage(Component.literal("§7[The Director] Событие §f" + found.id()
                + "§7 выполнено. Страх: §f" + memory.getDreadLevel()
                + "§7, безопасность: §f" + memory.getSafetyLevel()
                + "§7, акт: §f" + memory.getCurrentAct()));
        return 1;
    }

    // ------------------------------------------------------------------ dream

    /** Принудительный вход в измерение Сна (игнорирует шанс и правило «один сон в день»). */
    private static int enterDream(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        PlayerMemory memory = PlayerMemory.of(player);
        if (DreamDimension.isDream(player.level())) {
            context.getSource().sendSystemMessage(Component.literal("§7[The Director] Игрок уже во сне."));
            return 1;
        }
        boolean started = DreamDimension.tryEnter(player, memory, 0.0D, true);
        if (!started) {
            context.getSource().sendSystemMessage(Component.literal("§c[The Director] Сон не начался: измерение "
                    + "не загружено (нет датапака) или он отключён в конфиге (dream.enableDreamDimension)."));
            return 0;
        }
        context.getSource().sendSystemMessage(Component.literal("§7[The Director] Сон строится: копия мира "
                + "вокруг базы появится через несколько секунд."));
        return 1;
    }

    /** Выход из сна. */
    private static int leaveDream(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        PlayerMemory memory = PlayerMemory.of(player);
        if (!DreamDimension.isDream(player.level())) {
            context.getSource().sendSystemMessage(Component.literal("§7[The Director] Игрок не во сне."));
            return 0;
        }
        DreamDimension.leave(player, memory);
        context.getSource().sendSystemMessage(Component.literal("§7[The Director] Игрок разбужен."));
        return 1;
    }

    // ------------------------------------------------------------- вспомогательное

    private static void line(CommandSourceStack source, String label, String value) {
        source.sendSystemMessage(Component.literal("§8» §7" + label + ": §f" + value));
    }
}
