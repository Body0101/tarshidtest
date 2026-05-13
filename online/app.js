(function () {
  const cfg = window.TARSHID_CONFIG || {};
  let client = null;
  let session = null;
  let profile = null;
  let memberships = [];
  let devices = [];
  let currentDeviceId = null;
  let currentState = null;
  let channel = null;
  const events = [];

  function $(id) { return document.getElementById(id); }
  function escapeHtml(value) {
    return String(value ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }
  function toast(message) {
    const el = $("toast");
    if (!el) { alert(message); return; }
    el.textContent = message;
    el.classList.add("show");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => el.classList.remove("show"), 2600);
  }
  function configuredClient() {
    if (client) return client;
    if (!cfg.supabaseUrl || !cfg.supabaseAnonKey) {
      throw new Error("Supabase config.js is missing public URL/key.");
    }
    client = window.supabase.createClient(cfg.supabaseUrl, cfg.supabaseAnonKey, {
      auth: { persistSession: true, autoRefreshToken: true }
    });
    return client;
  }
  function page(path) {
    const base = location.pathname.replace(/[^/]*$/, "");
    return `${base}${path}`;
  }
  function roleIsAdmin() {
    return !!(profile && profile.global_role === "admin") || memberships.some((m) => m.role === "admin");
  }
  function setPill(id, text, kind) {
    const el = $(id);
    if (!el) return;
    el.textContent = text;
    el.className = `pill ${kind || ""}`.trim();
  }
  async function loadSession() {
    const supa = configuredClient();
    const { data } = await supa.auth.getSession();
    session = data.session;
    return session;
  }
  async function loadProfileAndDevices() {
    const supa = configuredClient();
    const user = session.user;
    const { data: profileRow, error: profileError } = await supa
      .from("smart_home_profiles")
      .select("id, username, display_name, global_role, active")
      .eq("id", user.id)
      .single();
    if (profileError) throw profileError;
    profile = profileRow;
    const { data: membershipRows, error: membershipError } = await supa
      .from("smart_home_device_memberships")
      .select("device_id, role, smart_home_devices(id, name)")
      .eq("user_id", user.id);
    if (membershipError) throw membershipError;
    memberships = membershipRows || [];
    devices = memberships.map((m) => ({ id: m.device_id, name: m.smart_home_devices?.name || m.device_id, role: m.role }));
    if (profile.global_role === "admin") {
      const { data: allDevices, error: devicesError } = await supa
        .from("smart_home_devices")
        .select("id, name")
        .eq("active", true)
        .order("name", { ascending: true });
      if (!devicesError && Array.isArray(allDevices)) {
        const seen = new Set(devices.map((d) => d.id));
        allDevices.forEach((d) => { if (!seen.has(d.id)) devices.push({ id: d.id, name: d.name || d.id, role: "admin" }); });
      }
    }
  }
  async function initLoginPage() {
    let supa;
    try { supa = configuredClient(); } catch (error) { $("loginMessage").textContent = error.message; return; }
    const { data } = await supa.auth.getSession();
    if (data.session) {
      session = data.session;
      await loadProfileAndDevices();
      location.replace(page(roleIsAdmin() ? "admin.html" : "simple.html"));
      return;
    }
    $("loginForm").addEventListener("submit", async (event) => {
      event.preventDefault();
      const username = $("username").value.trim();
      const password = $("password").value;
      $("loginMessage").textContent = "Checking credentials...";
      try {
        const { data: email, error: emailError } = await supa.rpc("auth_email_for_username", { p_username: username });
        if (emailError) throw emailError;
        if (!email) throw new Error("Username not found or inactive.");
        const { data: authData, error: authError } = await supa.auth.signInWithPassword({ email, password });
        if (authError) throw authError;
        session = authData.session;
        await loadProfileAndDevices();
        location.replace(page(roleIsAdmin() ? "admin.html" : "simple.html"));
      } catch (error) {
        $("loginMessage").textContent = error.message || "Login failed.";
      }
    });
  }
  async function initDashboard(options) {
    try {
      if (!await loadSession()) { location.replace(page("index.html")); return; }
      await loadProfileAndDevices();
    } catch (error) {
      toast(error.message || "Could not load session.");
      location.replace(page("index.html"));
      return;
    }
    if (options.requiredRole === "admin" && !roleIsAdmin()) {
      location.replace(page("simple.html"));
      return;
    }
    if (options.redirectAdminToAdvanced && roleIsAdmin()) {
      location.replace(page("admin.html"));
      return;
    }
    setPill("sessionPill", `${profile.display_name || profile.username} · ${roleIsAdmin() ? "Admin" : "User"}`, "good");
    bindDashboardEvents();
    renderDeviceSelect();
    if (devices.length > 0) await selectDevice(devices[0].id);
    else setPill("devicePill", "No assigned devices", "danger");
  }
  function bindDashboardEvents() {
    $("logoutBtn")?.addEventListener("click", async () => {
      await configuredClient().auth.signOut();
      location.replace(page("index.html"));
    });
    $("refreshBtn")?.addEventListener("click", () => loadDeviceState(true));
    $("deviceSelect")?.addEventListener("change", (event) => selectDevice(event.target.value));
    $("startTimerBtn")?.addEventListener("click", () => {
      const channelValue = Number($("timerChannel")?.value);
      const minutes = Number($("timerMinutes")?.value || 0);
      const target = $("timerTarget")?.value || "ON";
      if (!Number.isInteger(channelValue) || minutes < 1) { toast("Choose a relay and duration."); return; }
      sendCommand({ type: "set_timer", channel: channelValue, durationMinutes: minutes, target, epoch: Math.floor(Date.now() / 1000) });
    });
    $("cancelTimerBtn")?.addEventListener("click", () => {
      const channelValue = Number($("timerChannel")?.value);
      if (Number.isInteger(channelValue)) sendCommand({ type: "cancel_timer", channel: channelValue });
    });
    $("energyTrackingToggle")?.addEventListener("change", (event) => sendCommand({ type: "set_energy_tracking", enabled: event.target.checked }));
    $("resetConsumptionBtn")?.addEventListener("click", () => sendCommand({ type: "reset_consumption" }));
    $("saveMappingBtn")?.addEventListener("click", saveMapping);
  }
  function renderDeviceSelect() {
    const select = $("deviceSelect");
    if (!select) return;
    select.innerHTML = devices.map((d) => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name)} (${escapeHtml(d.role)})</option>`).join("");
  }
  async function selectDevice(deviceId) {
    currentDeviceId = deviceId;
    const select = $("deviceSelect");
    if (select) select.value = deviceId;
    const device = devices.find((d) => d.id === deviceId);
    setPill("devicePill", device ? device.name : deviceId, "good");
    subscribeToDevice(deviceId);
    await loadDeviceState(true);
  }
  function subscribeToDevice(deviceId) {
    const supa = configuredClient();
    if (channel) supa.removeChannel(channel);
    channel = supa.channel(`device-${deviceId}`)
      .on("postgres_changes", { event: "*", schema: "public", table: "smart_home_device_states", filter: `device_id=eq.${deviceId}` }, (payload) => {
        currentState = payload.new?.state || currentState;
        renderAll();
      })
      .on("postgres_changes", { event: "INSERT", schema: "public", table: "smart_home_device_events", filter: `device_id=eq.${deviceId}` }, (payload) => {
        addEvent(payload.new);
      })
      .on("postgres_changes", { event: "UPDATE", schema: "public", table: "smart_home_remote_commands", filter: `device_id=eq.${deviceId}` }, (payload) => {
        if (payload.new?.status === "done" || payload.new?.status === "failed") {
          const ok = payload.new.status === "done";
          toast(payload.new.result?.msg || (ok ? "Command applied." : "Command failed."));
        }
      })
      .subscribe((status) => setPill("syncPill", status === "SUBSCRIBED" ? "Realtime connected" : `Realtime ${status}`, status === "SUBSCRIBED" ? "good" : "warn"));
  }
  async function loadDeviceState(showMessage) {
    if (!currentDeviceId) return;
    const { data, error } = await configuredClient()
      .from("smart_home_device_states")
      .select("state, updated_at")
      .eq("device_id", currentDeviceId)
      .maybeSingle();
    if (error) { toast(error.message); return; }
    currentState = data?.state || currentState || null;
    renderAll();
    await loadRecentEvents();
    if (showMessage) setPill("syncPill", data ? `State ${new Date(data.updated_at).toLocaleTimeString()}` : "Waiting for ESP32", data ? "good" : "warn");
  }
  async function loadRecentEvents() {
    if (!currentDeviceId || !$("eventList")) return;
    const { data, error } = await configuredClient()
      .from("smart_home_device_events")
      .select("event, event_ts, payload, created_at")
      .eq("device_id", currentDeviceId)
      .order("created_at", { ascending: false })
      .limit(60);
    if (!error && Array.isArray(data)) {
      events.length = 0;
      data.reverse().forEach(addEvent);
      renderEvents();
    }
  }
  async function sendCommand(command) {
    if (!currentDeviceId) { toast("Select a device first."); return; }
    const { error } = await configuredClient().from("smart_home_remote_commands").insert({
      device_id: currentDeviceId,
      user_id: session.user.id,
      command
    });
    if (error) toast(error.message);
    else toast("Command queued for ESP32.");
  }
  function relays() { return Array.isArray(currentState?.relays) ? currentState.relays : []; }
  function pirs() { return Array.isArray(currentState?.pirs) ? currentState.pirs : []; }
  function relayName(relay, index) { return relay?.name || `Relay ${index + 1}`; }
  function renderAll() {
    renderRelays(); renderTimerSelect(); renderPower(); renderStats(); renderMapping(); renderSensors(); updateNightLockUi();
  }
  function renderRelays() {
    const list = $("relayList");
    if (!list) return;
    if (!currentState) { list.innerHTML = `<p class="muted">Waiting for ESP32 state...</p>`; return; }
    list.innerHTML = relays().map((relay, fallbackIndex) => {
      const index = Number.isInteger(Number(relay.index)) ? Number(relay.index) : fallbackIndex;
      const isOn = relay.state === "ON";
      const timer = relay.timerActive ? `Timer until ${new Date(Number(relay.timerEnd || 0) * 1000).toLocaleTimeString()}` : "No active timer";
      return `<article class="relay-card">
        <header><div><h3>${escapeHtml(relayName(relay, fallbackIndex))}</h3><small>Mode ${escapeHtml(relay.manualMode || "--")} · Source ${escapeHtml(relay.source || "--")}</small></div><div class="relay-state" style="color:${isOn ? "var(--good)" : "var(--muted)"}">${escapeHtml(relay.state || "OFF")}</div></header>
        <div class="muted">${escapeHtml(timer)}</div>
        <div class="actions">
          <button class="primary" ${currentState.nightLock ? "disabled" : ""} data-cmd="manual" data-channel="${index}" data-mode="ON">Manual ON</button>
          <button data-cmd="manual" data-channel="${index}" data-mode="OFF">Manual OFF</button>
          <button data-cmd="manual" data-channel="${index}" data-mode="AUTO">AUTO</button>
        </div>
      </article>`;
    }).join("");
    list.querySelectorAll("[data-cmd='manual']").forEach((button) => {
      button.addEventListener("click", () => sendCommand({ type: "set_manual", channel: Number(button.dataset.channel), mode: button.dataset.mode }));
    });
  }
  function renderTimerSelect() {
    const select = $("timerChannel");
    if (!select) return;
    const previous = select.value;
    select.innerHTML = relays().map((relay, index) => `<option value="${Number(relay.index ?? index)}">${escapeHtml(relayName(relay, index))}</option>`).join("");
    if ([...select.options].some((o) => o.value === previous)) select.value = previous;
  }
  function renderPower() {
    const list = $("powerList");
    if (!list) return;
    list.innerHTML = relays().map((relay, fallbackIndex) => {
      const index = Number(relay.index ?? fallbackIndex);
      return `<article class="relay-card">
        <header><div><h3>${escapeHtml(relayName(relay, fallbackIndex))}</h3><small>${Number(relay.totalEnergyWh || 0).toFixed(3)} Wh total</small></div></header>
        <label>Rated Power (W)<input id="power-${index}" type="number" min="0.1" step="0.1" value="${Number(relay.powerW || 0).toFixed(1)}" ${relay.powerLocked || currentState.nightLock ? "disabled" : ""}></label>
        <button ${relay.powerLocked || currentState.nightLock ? "disabled" : ""} data-rated="${index}" class="primary">Save Rated Power</button>
      </article>`;
    }).join("");
    list.querySelectorAll("[data-rated]").forEach((button) => button.addEventListener("click", () => {
      const index = Number(button.dataset.rated);
      const powerW = Number($(`power-${index}`).value || 0);
      if (powerW > 0) sendCommand({ type: "set_rated_power", channel: index, powerW });
    }));
    const toggle = $("energyTrackingToggle");
    if (toggle) toggle.checked = !!currentState?.energyTrackingEnabled;
  }
  function renderStats() {
    const list = $("statsList");
    if (!list) return;
    list.innerHTML = relays().map((relay, index) => `<article class="event-row"><span>${escapeHtml(relayName(relay, index))}</span><strong>${Number(relay.timerUses || 0)} uses · ${Number(relay.totalTimerMinutes || 0)} min</strong></article>`).join("");
  }
  function renderMapping() {
    const list = $("mappingList");
    if (!list) return;
    list.innerHTML = pirs().map((pir, pirIndex) => `<article class="mapping-row"><strong>${escapeHtml(pir.name || `PIR ${pirIndex + 1}`)}</strong><div class="checkbox-row">${relays().map((relay, relayIndex) => {
      const checked = Array.isArray(pir.relays) ? !!pir.relays[relayIndex] : !!(Number(pir.relayMask || 0) & (1 << relayIndex));
      return `<label><input type="checkbox" data-pir="${pirIndex}" data-relay="${relayIndex}" ${checked ? "checked" : ""} ${currentState.nightLock ? "disabled" : ""}> ${escapeHtml(relayName(relay, relayIndex))}</label>`;
    }).join("")}</div></article>`).join("");
  }
  function saveMapping() {
    const mappings = pirs().map((pir, pirIndex) => ({
      relays: relays().map((_, relayIndex) => !!document.querySelector(`[data-pir="${pirIndex}"][data-relay="${relayIndex}"]`)?.checked)
    }));
    sendCommand({ type: "set_pir_mapping", mappings });
  }
  function renderSensors() {
    const list = $("sensorList");
    if (!list) return;
    list.innerHTML = pirs().map((pir, index) => `<article class="sensor-card ${pir.value ? "active" : ""}"><span>${escapeHtml(pir.name || `PIR ${index + 1}`)}</span><strong>${pir.value ? "Active" : "Idle"}</strong></article>`).join("");
  }
  function addEvent(row) {
    if (!row) return;
    events.push(row);
    while (events.length > 80) events.shift();
    renderEvents();
  }
  function renderEvents() {
    const list = $("eventList");
    if (!list) return;
    list.innerHTML = events.slice().reverse().map((row) => {
      const payload = row.payload || {};
      const label = payload.msg || payload.message || row.event || "event";
      const ts = row.event_ts ? new Date(Number(row.event_ts) * 1000).toLocaleString() : new Date(row.created_at).toLocaleString();
      return `<article class="event-row"><span>${escapeHtml(label)}</span><small>${escapeHtml(ts)}</small></article>`;
    }).join("");
  }
  function updateNightLockUi() {
    const locked = !!currentState?.nightLock;
    if ($("startTimerBtn")) $("startTimerBtn").disabled = locked;
    if ($("timerTarget")) $("timerTarget").disabled = locked;
    if ($("saveMappingBtn")) $("saveMappingBtn").disabled = locked;
    setPill("syncPill", currentState ? `Night Lock ${locked ? "ON" : "OFF"}` : "Waiting for state", locked ? "warn" : "good");
  }

  window.TarshidOnline = { initLoginPage, initDashboard };
})();


