/* AI Agent Studio — веб-интерфейс. Только fetch и EventSource, без фреймворков. */
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const chat = $("chat");
  const input = $("input");

  const state = { busy: false, current: null, providers: {}, abort: null };

  const escapeHtml = (text) => String(text ?? "").replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]
  ));

  const render = (text) => {
    let out = escapeHtml(text);
    out = out.replace(/```(\w+)?\n([\s\S]*?)```/g, (_, lang, code) =>
      `<pre><code data-lang="${escapeHtml(lang || "")}">${code}</code></pre>`);
    out = out.replace(/`([^`\n]+)`/g, "<code>$1</code>");
    out = out.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    out = out.replace(/(https?:\/\/[^\s<)]+)/g, '<a href="$1" target="_blank" rel="noreferrer">$1</a>');
    return out;
  };

  const scroll = (force = false) => {
    const nearBottom = chat.scrollHeight - chat.scrollTop - chat.clientHeight < 200;
    if (force || nearBottom) chat.scrollTop = chat.scrollHeight;
  };

  const addNode = (html, className) => {
    const node = document.createElement("div");
    node.className = className || "msg assistant";
    node.innerHTML = html;
    chat.appendChild(node);
    scroll(true);
    return node;
  };

  const system = (text) => addNode(escapeHtml(text), "msg system");
  const clearWelcome = () => chat.querySelector(".welcome")?.remove();

  /* ------------------------------------------------------------ запросы */
  async function api(path, options = {}) {
    const response = await fetch(path, {
      headers: { "Content-Type": "application/json" },
      ...options,
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || `${response.status} ${response.statusText}`);
    return data;
  }

  async function refresh() {
    try {
      const [health, config] = await Promise.all([api("/api/health"), api("/api/config")]);
      $("version").textContent = `v${health.version}`;
      $("st-provider").textContent = health.provider_title || health.provider;
      $("st-model").textContent = health.model;
      $("st-tools").textContent = health.tools;
      $("st-workspace").textContent = health.workspace;
      const pill = $("health");
      pill.textContent = health.ok ? "готов" : (health.message || "нет связи");
      pill.className = "pill " + (health.ok ? "ok" : "err");
      $("chat-sub").textContent = `${health.model} · режим ${health.mode}`;

      state.providers = config.providers || {};
      const providerSelect = $("provider");
      if (!providerSelect.options.length) {
        Object.entries(state.providers).forEach(([name, info]) => {
          const option = document.createElement("option");
          option.value = name;
          option.textContent = `${info.title}${info.local === "да" ? " (локально)" : ""}`;
          providerSelect.appendChild(option);
        });
      }
      providerSelect.value = health.provider;
      $("model").value = config.config.model || "";
      $("model").placeholder = health.model;
      $("mode").value = config.config.mode;
      $("max-steps").value = config.config.max_steps;
      updateProviderHint();

      const hw = config.hardware || {};
      $("hardware").innerHTML = [
        `CPU: ${hw.cpu_cores} потоков`,
        `RAM: ${hw.ram_gb} ГБ`,
        hw.gpu_name ? `GPU: ${escapeHtml(hw.gpu_name)} (${hw.vram_gb} ГБ VRAM)` : "GPU: нет",
        `Рекомендуется: <b>${escapeHtml((config.recommendation || {}).model || "—")}</b>`,
        `контекст: ${(config.recommendation || {}).num_ctx || "—"} токенов`,
      ].join("<br>");

      loadTools();
      loadHistory();
      loadSessions();
      loadMcp();
    } catch (error) {
      $("health").textContent = "сервер недоступен: " + error.message;
      $("health").className = "pill err";
    }
  }

  function updateProviderHint() {
    const info = state.providers[$("provider").value];
    if (!info) return;
    const parts = [`модель по умолчанию: ${info.default_model}`];
    if (info.needs_key !== "нет") parts.push(`ключ: ${info.needs_key} (${info.key_present})`);
    $("provider-hint").textContent = parts.join(" · ");
  }

  async function loadTools() {
    try {
      const data = await api("/api/tools");
      $("tools").innerHTML = data.tools.map((tool) =>
        `<div><b>${escapeHtml(tool.name)}</b>${tool.dangerous ? " ⚠" : ""} — ${escapeHtml(tool.description)}</div>`
      ).join("");
    } catch { /* игнорируем */ }
  }

  async function loadHistory() {
    try {
      const data = await api("/api/history");
      $("history").innerHTML = escapeHtml(data.history || "пусто").replace(/\n/g, "<br>");
    } catch { /* игнорируем */ }
  }

  async function loadSessions() {
    try {
      const data = await api("/api/sessions");
      $("sessions").innerHTML = data.sessions.length
        ? data.sessions.slice(0, 10).map((s) =>
            `<div>${escapeHtml(s.name)} <span class="small">(${s.messages})</span></div>`).join("")
        : "<div>пока нет</div>";
    } catch { /* игнорируем */ }
  }

  async function loadMcp() {
    try {
      const data = await api("/api/mcp");
      $("mcp").innerHTML = escapeHtml(data.servers || "не настроены").replace(/\n/g, "<br>");
    } catch { /* игнорируем */ }
  }

  /* ------------------------------------------------------------ отправка задачи */
  function setBusy(busy) {
    state.busy = busy;
    $("busy").classList.toggle("hidden", !busy);
    $("send").disabled = busy;
    $("stop").disabled = !busy;
  }

  async function sendTask(text) {
    if (state.busy || !text.trim()) return;
    clearWelcome();
    addNode(`<div class="role">вы</div><div class="content">${render(text)}</div>`, "msg user");
    input.value = "";
    input.style.height = "auto";
    setBusy(true);
    state.current = null;

    const controller = new AbortController();
    state.abort = controller;

    try {
      const response = await fetch("/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          message: text,
          provider: $("provider").value || undefined,
          model: $("model").value.trim() || undefined,
          mode: $("mode").value,
          max_steps: Number($("max-steps").value) || undefined,
        }),
        signal: controller.signal,
      });
      if (!response.ok || !response.body) throw new Error(`${response.status} ${response.statusText}`);

      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        let index;
        while ((index = buffer.indexOf("\n\n")) >= 0) {
          const chunk = buffer.slice(0, index).trim();
          buffer = buffer.slice(index + 2);
          if (chunk.startsWith("data:")) {
            try { handleEvent(JSON.parse(chunk.slice(5).trim())); } catch { /* пропускаем */ }
          }
        }
      }
    } catch (error) {
      if (error.name !== "AbortError") system("Ошибка соединения: " + error.message);
    } finally {
      setBusy(false);
      state.abort = null;
      if (state.current) state.current.classList.remove("cursor");
      state.current = null;
      refresh();
    }
  }

  /* ------------------------------------------------------------ события агента */
  function handleEvent(event) {
    switch (event.type) {
      case "start":
        $("chat-sub").textContent = `${event.model} · ${event.provider}`;
        break;

      case "text": {
        if (!state.current) {
          const node = addNode('<div class="role">агент</div><div class="content cursor"></div>', "msg assistant");
          state.current = node.querySelector(".content");
          state.current.dataset.raw = "";
        }
        state.current.dataset.raw += event.text;
        state.current.innerHTML = render(state.current.dataset.raw);
        scroll();
        break;
      }

      case "thinking": {
        let node = chat.querySelector(".thinking");
        if (!node) node = addNode('<div class="role">размышления</div><div class="content"></div>', "msg assistant thinking");
        node.querySelector(".content").textContent += event.text;
        scroll();
        break;
      }

      case "step":
        if (event.meta && event.meta.step > 1) addNode(`шаг ${event.meta.step}/${event.meta.limit}`, "step");
        break;

      case "tool_start": {
        const args = JSON.stringify(event.call?.arguments ?? {}, null, 2);
        const node = addNode(
          `<div class="head">🔧 <b>${escapeHtml(event.name)}</b><span class="status">выполняется…</span></div>` +
          `<pre>${escapeHtml(args)}</pre>`, "tool");
        node.dataset.name = event.name;
        state.current = null;
        break;
      }

      case "tool_end": {
        const nodes = [...chat.querySelectorAll(".tool")];
        const node = nodes[nodes.length - 1];
        if (!node) break;
        node.classList.add(event.ok ? "ok" : "err");
        const status = node.querySelector(".status");
        status.textContent = (event.ok ? "✔ " : "✘ ") + (event.display || event.name) +
          (event.seconds ? ` · ${event.seconds} с` : "");
        const pre = node.querySelector("pre");
        pre.textContent += "\n— — —\n" + (event.content || "");
        break;
      }

      case "compact":
        system("Контекст сжат: " + (event.text || "").slice(0, 400));
        break;

      case "info":
        if (event.text) system(event.text);
        else if (event.meta && event.meta.tok_per_s) system(`скорость: ${event.meta.tok_per_s} ток/с`);
        break;

      case "warn":
        system("⚠ " + event.text);
        break;

      case "error":
        system("⛔ " + event.text);
        if (state.current) { state.current.classList.remove("cursor"); state.current = null; }
        break;

      case "done":
        if (state.current) state.current.classList.remove("cursor");
        state.current = null;
        if (event.meta) {
          const parts = [];
          if (event.meta.steps) parts.push(`шагов ${event.meta.steps}`);
          if (event.meta.tool_calls) parts.push(`инструментов ${event.meta.tool_calls}`);
          if (event.meta.duration) parts.push(`${Number(event.meta.duration).toFixed(1)} с`);
          if (event.meta.usage_line) parts.push(event.meta.usage_line);
          if (parts.length) system(parts.join(" · "));
          if (event.meta.files_changed?.length) {
            system("изменённые файлы: " + event.meta.files_changed.join(", "));
          }
        }
        break;

      case "end":
        setBusy(false);
        break;

      default:
        break;
    }

    if (event.pending && event.pending.id) showConfirm(event.pending);
  }

  function showConfirm(pending) {
    $("modal-title").textContent = pending.title || "Подтверждение";
    $("modal-body").textContent = pending.description || "";
    $("modal").classList.remove("hidden");
    const answer = (allow) => {
      $("modal").classList.add("hidden");
      api("/api/confirm", { method: "POST", body: JSON.stringify({ id: pending.id, allow }) })
        .catch(() => {});
    };
    $("allow").onclick = () => answer(true);
    $("deny").onclick = () => answer(false);
  }

  /* ------------------------------------------------------------ обработчики UI */
  $("send").onclick = () => sendTask(input.value);
  input.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      sendTask(input.value);
    }
  });
  input.addEventListener("input", () => {
    input.style.height = "auto";
    input.style.height = Math.min(input.scrollHeight, 220) + "px";
  });

  $("stop").onclick = () => api("/api/stop", { method: "POST" }).catch(() => {});
  $("new-chat").onclick = () => {
    chat.innerHTML = "";
    system("Начинаем новый диалог.");
    api("/api/reset", { method: "POST" }).catch(() => {});
  };

  $("apply").onclick = async () => {
    try {
      await api("/api/config", {
        method: "POST",
        body: JSON.stringify({
          provider: $("provider").value,
          model: $("model").value.trim(),
          mode: $("mode").value,
          max_steps: Number($("max-steps").value),
          save: true,
          rebuild: true,
        }),
      });
      system(`Настройки применены: ${$("provider").value} / ${$("model").value || "по умолчанию"} / режим ${$("mode").value}`);
      refresh();
    } catch (error) {
      system("Не удалось применить настройки: " + error.message);
    }
  };

  $("provider").onchange = updateProviderHint;

  $("undo").onclick = async () => {
    try {
      const data = await api("/api/undo", { method: "POST", body: JSON.stringify({ count: 1 }) });
      system(data.result || "откат выполнен");
      loadHistory();
    } catch (error) {
      system("Ошибка отката: " + error.message);
    }
  };

  document.querySelectorAll(".chip").forEach((chip) => {
    chip.onclick = () => sendTask(chip.textContent);
  });

  refresh();
  setInterval(refresh, 25000);
})();
