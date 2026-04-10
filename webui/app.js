(function () {
    const KEY_BASE = "px.api.base";
    const KEY_DEMO = "px.demo.mode";

    function el(id) {
        return document.getElementById(id);
    }

    function nowIso() {
        return new Date().toISOString();
    }

    function getCurrentOriginBase() {
        if (window.location.protocol === "http:" || window.location.protocol === "https:") {
            return window.location.origin;
        }
        return "";
    }

    function getApiBase() {
        const originBase = getCurrentOriginBase();
        if (originBase) {
            return originBase;
        }
        return localStorage.getItem(KEY_BASE) || "http://192.168.4.1";
    }

    function getDemoMode() {
        return localStorage.getItem(KEY_DEMO) === "1";
    }

    function setApiBase(value) {
        localStorage.setItem(KEY_BASE, value.trim());
    }

    function setDemoMode(enabled) {
        localStorage.setItem(KEY_DEMO, enabled ? "1" : "0");
    }

    async function api(path, options) {
        if (getDemoMode()) {
            return mockResponse(path, options);
        }

        const originBase = getCurrentOriginBase();
        const base = getApiBase().replace(/\/$/, "");
        const requestUrl = originBase ? path : (base + path);
        const res = await fetch(requestUrl, options);
        if (!res.ok) {
            throw new Error(`HTTP ${res.status} ${res.statusText}`);
        }
        return res.json();
    }

    async function mockResponse(path, options) {
        await new Promise((r) => setTimeout(r, 120));

        if (path === "/api/state") {
            return {
                ts: Date.now(),
                id: "px-wifi-v1",
                status: "online",
                gameState: "ready",
                timeRemaining: 3600,
                triesUsed: 0,
                maxTries: 3,
                mode: "penalty",
                battery: 100,
                batteryVoltageMv: 5200,
                batteryProfile: "unknown",
                lowBattery: false,
                version: "demo",
                buildId: "demo-local"
            };
        }

        if (path === "/api/config" || path === "/api/config/defaults") {
            return {
                defaultTime: 3600,
                penalty: 30,
                maxTries: 3,
                wireCount: 4,
                mode: "penalty",
                lidMode: "ignore",
                solution: "1234",
                keepSyncEnabled: false,
                heartbeatInterval: 10000,
                batteryProfile: "unknown",
                lowBatteryPercent: 40,
                batteryVoltageMv: 6500,
                input1Name: "red",
                input2Name: "green",
                input3Name: "yellow",
                input4Name: "blue",
                input5Name: "white",
                input6Name: "orange",
                input7Name: "brown",
                input8Name: "purple"
            };
        }

            if (path === "/api/device/details") {
                return {
                    propName: "px-wifi-v1-a1b2",
                    ipAddress: "192.168.4.1",
                    softwareVersion: "demo",
                    buildNumber: "demo-local",
                    buildDate: "2026-04-09 10:00:00",
                    cpuTempC: null,
                    freeMemoryBytes: 243712,
                    batteryPercent: 100,
                    networkName: "px-wifi-v1-a1b2",
                    status: "ready"
                };
            }

            if (path === "/api/device/name") {
                const payload = options && options.body ? JSON.parse(options.body) : {};
                const name = String(payload.networkName || "px-wifi-v1-a1b2").toLowerCase();
                return {
                    ok: true,
                    networkName: name,
                    url: `http://${name}.local`
                };
            }

        if (path === "/api/command") {
            const payload = options && options.body ? JSON.parse(options.body) : {};
            if (payload.command === "ping") {
                return { event: "pong", ts: Date.now() };
            }
            if (payload.command === "getState") {
                return mockResponse("/api/state");
            }
            return { ok: true, mock: true, command: payload.command || "unknown" };
        }

        if (path === "/api/config/save" || path === "/api/config" || path === "/api/config/restore" || path === "/api/config/restore/save") {
            return { ok: true, mock: true, persisted: path.endsWith("save") };
        }

        if (path === "/api/connection") {
            if (options && options.method === "POST") {
                return { ok: true, mock: true, applied: true };
            }
            return {
                wifiSsid: "Paradox-StageAP",
                wifiPassword: "",
                mqttHost: "192.168.1.50",
                mqttPort: 1883,
                mqttUsername: "",
                mqttPassword: "",
                mqttBaseTopic: "paradox",
                mqttCommandTopic: "paradox/site/zone/commands",
                mqttStateTopic: "paradox/site/zone/state",
                mqttEventsTopic: "paradox/site/zone/events",
                mqttWarningsTopic: "paradox/site/zone/warnings",
                mqttGameStateTopic: "paradox/game/state",
                mqttPropStateTopic: "paradox/state",
                networkName: "px-wifi-v1-a1b2",
                apPassword: "",
                apEnabled: true
            };
        }

        if (path === "/api/connection/scan") {
            return {
                ok: true,
                networks: [
                    { ssid: "Paradox-NOC", rssi: -42 },
                    { ssid: "Props-Backstage", rssi: -58 },
                    { ssid: "TMOBILE", rssi: -64 }
                ]
            };
        }

        return { ok: true, mock: true };
    }

    function formatTime(seconds) {
        const s = Math.max(0, Number(seconds) || 0);
        const mm = String(Math.floor(s / 60)).padStart(2, "0");
        const ss = String(s % 60).padStart(2, "0");
        return `${mm}:${ss}`;
    }

    function appendLog(node, value) {
        if (!node) {
            return;
        }
        node.textContent = `[${nowIso()}]\n${JSON.stringify(value, null, 2)}\n\n` + node.textContent;
    }

    function batteryMeta(state) {
        const profile = String(state.batteryProfile || "unknown");
        const voltageMv = Number(state.batteryVoltageMv || 0);
        const externalLike = profile === "external" || profile === "unknown";
        const low = externalLike ? voltageMv < 5000 : Boolean(state.lowBattery);
        const percent = externalLike ? 100 : Math.max(0, Math.min(100, Number(state.battery || 0)));

        return {
            profile,
            voltageMv,
            low,
            percent
        };
    }

    function tablerBatterySvg(percent, low) {
        const pct = Math.max(0, Math.min(100, Number(percent || 0)));
        const fill = Math.round((pct / 100) * 16);
        const color = low ? "#b57500" : "#2f8f74";

        return `<svg class="battery-svg" viewBox="0 0 24 24" aria-hidden="true"><rect x="2" y="7" width="18" height="10" rx="2" ry="2" fill="none" stroke="currentColor" stroke-width="1.8"/><rect x="20" y="10" width="2" height="4" rx="1" fill="currentColor"/><rect x="4" y="9" width="${fill}" height="6" rx="1" fill="${color}"/></svg>`;
    }

    function tablerWifiSvg(level) {
        const l = Math.max(1, Math.min(4, Number(level || 1)));
        const color = l >= 3 ? "#2f8f74" : l === 2 ? "#b57500" : "#b23a3a";
        const op1 = l >= 1 ? 1 : 0.25;
        const op2 = l >= 2 ? 1 : 0.25;
        const op3 = l >= 3 ? 1 : 0.25;
        const op4 = l >= 4 ? 1 : 0.25;

        return `<svg class="wifi-svg" viewBox="0 0 24 24" aria-hidden="true"><path d="M3 9.5a13 13 0 0 1 18 0" fill="none" stroke="${color}" stroke-opacity="${op4}" stroke-width="1.8" stroke-linecap="round"/><path d="M6 13a9 9 0 0 1 12 0" fill="none" stroke="${color}" stroke-opacity="${op3}" stroke-width="1.8" stroke-linecap="round"/><path d="M9 16.5a5 5 0 0 1 6 0" fill="none" stroke="${color}" stroke-opacity="${op2}" stroke-width="1.8" stroke-linecap="round"/><circle cx="12" cy="20" r="1.5" fill="${color}" fill-opacity="${op1}"/></svg>`;
    }

    function ensureBatteryBadge() {
        let badge = el("batteryBadge");
        if (badge) {
            return badge;
        }

        const container = el("statusIcons");
        if (!container) {
            return null;
        }

        badge = document.createElement("div");
        badge.id = "batteryBadge";
        badge.className = "battery-badge battery-good";
        badge.innerHTML = `<span id="batteryBadgeIcon" class="battery-icon"></span><span id="batteryBadgeText">--%</span>`;
        container.appendChild(badge);
        return badge;
    }

    function ensureWifiBadge() {
        let badge = el("wifiBadge");
        if (badge) {
            return badge;
        }

        const container = el("statusIcons");
        if (!container) {
            return null;
        }

        badge = document.createElement("div");
        badge.id = "wifiBadge";
        badge.className = "wifi-badge";
        badge.innerHTML = `<span id="wifiBadgeIcon" class="wifi-icon"></span><span id="wifiBadgeText">--</span>`;
        container.insertBefore(badge, container.firstChild);
        return badge;
    }

    function renderWifiStatus(details) {
        const badge = ensureWifiBadge();
        if (badge) {
            const icon = el("wifiBadgeIcon");
            const text = el("wifiBadgeText");
            if (details && details.wifiConnected) {
                const level = wifiLevel(details.wifiRssi);
                if (icon) { icon.innerHTML = tablerWifiSvg(level); }
                if (text) { text.textContent = details.wifiSsid || "Connected"; }
            } else {
                if (icon) { icon.innerHTML = tablerWifiSvg(0); }
                if (text) { text.textContent = "No WiFi"; }
            }
        }
    }

    var s_lastDeviceDetails = null;

    async function fetchStatusIcons() {
        try {
            const details = await api("/api/device/details");
            s_lastDeviceDetails = details;
            renderWifiStatus(details);
            if (details.batteryPercent != null) {
                renderBattery({ battery: details.batteryPercent, batteryProfile: "unknown", batteryVoltageMv: 0, lowBattery: false });
            }
        } catch (e) { /* silent */ }
    }

    function wifiLevel(rssi) {
        const v = Number(rssi);
        if (!Number.isFinite(v)) {
            return 0;
        }
        if (v >= -55) {
            return 4;
        }
        if (v >= -67) {
            return 3;
        }
        if (v >= -75) {
            return 2;
        }
        return 1;
    }

    function renderBattery(state) {
        const meta = batteryMeta(state);
        const badge = ensureBatteryBadge();

        if (badge) {
            badge.classList.toggle("battery-good", !meta.low);
            badge.classList.toggle("battery-warn", meta.low);

            const iconNode = el("batteryBadgeIcon");
            const textNode = el("batteryBadgeText");
            if (iconNode) {
                iconNode.innerHTML = tablerBatterySvg(meta.percent, meta.low);
            }
            if (textNode) {
                textNode.textContent = `${meta.percent}%`;
            }

            badge.title = `${meta.profile} @ ${(meta.voltageMv / 1000).toFixed(2)}V`;
        }

        const panelBattery = el("batteryValue");
        if (panelBattery) {
            panelBattery.textContent = `${meta.percent}% (${(meta.voltageMv / 1000).toFixed(2)}V)`;
            panelBattery.classList.toggle("text-ok", !meta.low);
            panelBattery.classList.toggle("text-warn", meta.low);
        }
    }

    function applyApiPrefsToPage() {
        const base = el("apiBase");
        if (base) {
            base.value = getApiBase();
        }

        const demo = el("demoMode");
        if (!demo) {
            return;
        }

        if (demo.tagName === "INPUT") {
            demo.checked = getDemoMode();
        } else if (demo.tagName === "SELECT") {
            demo.value = getDemoMode() ? "1" : "0";
        }
    }

    function bindApiPrefs(logNode) {
        const save = el("saveApi");
        const base = el("apiBase");
        const demo = el("demoMode");

        if (!save || !base || !demo) {
            return;
        }

        save.addEventListener("click", () => {
            setApiBase(base.value);
            const enabled = demo.tagName === "SELECT" ? demo.value === "1" : demo.checked;
            setDemoMode(enabled);
            appendLog(logNode, { apiBase: getApiBase(), demoMode: getDemoMode() });
        });
    }

    function pageDashboard() {
        const log = el("actionLog");

        async function refreshState() {
            try {
                const state = await api("/api/state");
                const stateText = String(state.gameState || "unknown");
                el("stateBadge").textContent = stateText;
                el("timeRemaining").textContent = formatTime(state.timeRemaining);
                el("triesUsed").textContent = `${state.triesUsed ?? "-"} / ${state.maxTries ?? "-"}`;
                const showTries = state.mode === "buzz" || state.mode === "penalty";
                const triesMetric = el("triesMetric");
                if (triesMetric) {
                    triesMetric.classList.toggle("hidden", !showTries);
                }
                el("mode").textContent = stateText === "ready" || stateText === "not_ready" ? stateText : "not_ready";
                renderBattery(state);
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        }

        el("refreshBtn").addEventListener("click", refreshState);
        document.querySelectorAll("[data-cmd]").forEach((btn) => {
            btn.addEventListener("click", async () => {
                try {
                    const payload = JSON.parse(btn.getAttribute("data-cmd"));
                    const result = await api("/api/command", {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify(payload)
                    });
                    appendLog(log, { command: payload, result: result });
                    await refreshState();
                } catch (err) {
                    appendLog(log, { error: String(err) });
                }
            });
        });

        refreshState();
        fetchStatusIcons();
        setInterval(refreshState, 3000);
        setInterval(fetchStatusIcons, 10000);
    }

    function pageConfig() {
        const form = el("configForm");
        const log = el("configLog");

        function updateModeDependencies() {
            const mode = String(form.elements.mode.value || "penalty");
            const maxTries = el("maxTries");
            const penalty = el("penalty");

            if (maxTries) {
                maxTries.disabled = mode === "instant";
            }
            if (penalty) {
                penalty.disabled = mode !== "penalty";
            }
        }

        function normalizeSolutionInput() {
            const field = form.elements.solution;
            if (!field) {
                return;
            }
            field.value = String(field.value || "").replace(/\s+/g, "").replace(/[^1-8]/g, "").slice(0, 8);
        }

        function formToPayload() {
            const data = new FormData(form);
            const wireCount = Number(data.get("wireCount"));
            const rawSolution = String(data.get("solution") || "");
            const cleanSolution = rawSolution.replace(/\s+/g, "").replace(/[^1-8]/g, "").slice(0, 8);
            const payload = {
                defaultTime: Number(data.get("defaultTime")),
                lidMode: String(data.get("lidMode")),
                keepSyncEnabled: String(data.get("keepSyncEnabled")) === "true",
                mode: String(data.get("mode")),
                maxTries: Number(data.get("maxTries")),
                penalty: Number(data.get("penalty")),
                wireCount: wireCount,
                solution: cleanSolution,
                heartbeatInterval: Number(data.get("heartbeatInterval")),
                batteryProfile: String(data.get("batteryProfile")),
                lowBatteryPercent: Number(data.get("lowBatteryPercent")),
                input1Name: String(data.get("input1Name")),
                input2Name: String(data.get("input2Name")),
                input3Name: String(data.get("input3Name")),
                input4Name: String(data.get("input4Name")),
                input5Name: String(data.get("input5Name")),
                input6Name: String(data.get("input6Name")),
                input7Name: String(data.get("input7Name")),
                input8Name: String(data.get("input8Name"))
            };

            if (payload.mode === "instant") {
                delete payload.maxTries;
            }
            if (payload.mode !== "penalty") {
                delete payload.penalty;
            }

            return payload;
        }

        function fillForm(cfg) {
            Object.keys(cfg).forEach((key) => {
                if (form.elements[key]) {
                    if (typeof cfg[key] === "boolean") {
                        form.elements[key].value = cfg[key] ? "true" : "false";
                    } else {
                        form.elements[key].value = String(cfg[key]);
                    }
                }
            });
            updateModeDependencies();
        }

        async function load() {
            try {
                const cfg = await api("/api/config");
                fillForm(cfg);
                appendLog(log, { loaded: true, config: cfg });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        }

        async function restoreDefaults(persist) {
            try {
                const path = persist ? "/api/config/restore/save" : "/api/config/restore";
                const result = await api(path, {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: "{}"
                });
                appendLog(log, { restoreDefaults: true, persisted: persist, result: result });
                await load();
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        }

        form.elements.mode.addEventListener("change", updateModeDependencies);
        form.elements.solution.addEventListener("input", normalizeSolutionInput);

        el("sendRaw").addEventListener("click", async () => {
            const raw = el("rawCommand");
            const rawLog = el("rawLog");
            try {
                const payload = JSON.parse(raw.value);
                const result = await api("/api/command", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(rawLog, { command: payload, result: result });
            } catch (err) {
                appendLog(rawLog, { error: String(err) });
            }
        });

        el("applyConfig").addEventListener("click", async () => {
            try {
                const payload = formToPayload();
                const result = await api("/api/config", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(log, { apply: payload, result: result });
                await load();
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        });

        el("saveConfig").addEventListener("click", async () => {
            try {
                const payload = formToPayload();
                const result = await api("/api/config/save", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(log, { save: payload, result: result });
                await load();
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        });

        el("restoreDefaults").addEventListener("click", () => restoreDefaults(false));

        load();
        fetchStatusIcons();
        setInterval(fetchStatusIcons, 10000);
    }

    function pageConnection() {
        const log = el("connectionLog");
        const deviceLog = el("deviceLog");
        const ssidList = el("ssidList");

        function fillConnection(cfg) {
            el("wifiSsid").value = cfg.wifiSsid || "";
            el("wifiPassword").value = cfg.wifiPassword || "";
            el("mqttHost").value = cfg.mqttHost || "";
            el("mqttPort").value = cfg.mqttPort || 1883;
            el("mqttUsername").value = cfg.mqttUsername || "";
            el("mqttPassword").value = cfg.mqttPassword || "";
            el("mqttBaseTopic").value = cfg.mqttBaseTopic || "paradox";
            el("mqttCommandTopic").value = cfg.mqttCommandTopic || "";
            el("mqttStateTopic").value = cfg.mqttStateTopic || "";
            el("mqttEventsTopic").value = cfg.mqttEventsTopic || "";
            el("mqttWarningsTopic").value = cfg.mqttWarningsTopic || "";
            el("mqttGameStateTopic").value = cfg.mqttGameStateTopic || "";
            el("mqttPropStateTopic").value = cfg.mqttPropStateTopic || "";
            el("networkName").value = cfg.networkName || "";
            if (el("apPassword")) {
                el("apPassword").value = cfg.apPassword || "";
            }
            if (el("apEnabled")) {
                el("apEnabled").checked = cfg.apEnabled !== false;
            }
        }

        function setText(id, value) {
            const node = el(id);
            if (node) {
                node.textContent = value;
            }
        }

        function renderDeviceDetails(details) {
            setText("detailPropName", details.propName || "-");
            setText("detailIpAddress", details.ipAddress || "-");
            setText("detailSoftwareVersion", details.softwareVersion || "-");
            setText("detailBuildNumber", details.buildNumber || "-");
            setText("detailBuildDate", details.buildDate || "-");
            setText("detailCpuTemp", details.cpuTempC == null ? "n/a" : `${details.cpuTempC.toFixed(1)} C`);
            setText("detailFreeMemory", details.freeMemoryBytes == null ? "-" : `${Math.round(details.freeMemoryBytes / 1024)} KB`);
            setText("detailBattery", details.batteryPercent == null ? "-" : `${details.batteryPercent}%`);

            if (details.networkName && el("networkName") && document.activeElement !== el("networkName")) {
                el("networkName").value = details.networkName;
            }
        }

        function renderWifiConnectionStatus(details) {
            const statusEl = el("wifiStatus");
            if (!statusEl) { return; }
            if (details && details.wifiConnected) {
                const level = wifiLevel(details.wifiRssi);
                statusEl.innerHTML = `<span class="wifi-icon">${tablerWifiSvg(level)}</span> Connected to <strong>${details.wifiSsid || "?"}</strong> (${details.wifiRssi} dBm)`;
                statusEl.style.color = "var(--ok)";
            } else {
                statusEl.innerHTML = `Not connected to any network`;
                statusEl.style.color = "var(--muted)";
            }
        }

        function renderSsidList(networks) {
            ssidList.innerHTML = "";
            if (!Array.isArray(networks) || networks.length === 0) {
                ssidList.innerHTML = "<p class='hint'>No SSIDs found.</p>";
                return;
            }

            networks.sort((a, b) => (b.rssi || -999) - (a.rssi || -999));
            networks.forEach((n) => {
                const b = document.createElement("button");
                const level = wifiLevel(n.rssi);
                b.className = "ssid-item";
                b.type = "button";
                b.innerHTML = `<span>${n.ssid || "<hidden>"}</span><span class="ssid-meta"><span class="wifi-icon">${tablerWifiSvg(level)}</span>${n.rssi ?? "?"} dBm</span>`;
                b.addEventListener("click", () => {
                    el("wifiSsid").value = n.ssid || "";
                });
                ssidList.appendChild(b);
            });
        }

        async function loadConnection() {
            try {
                const cfg = await api("/api/connection");
                fillConnection(cfg);
                appendLog(log, { loaded: true, connection: cfg });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        }

        async function loadDeviceDetails() {
            try {
                const details = await api("/api/device/details");
                renderDeviceDetails(details);
                renderWifiConnectionStatus(details);
                renderWifiStatus(details);
                if (details.batteryPercent != null) {
                    renderBattery({ battery: details.batteryPercent, batteryProfile: "unknown", batteryVoltageMv: 0, lowBattery: false });
                }
            } catch (err) {
                appendLog(deviceLog, { error: String(err) });
            }
        }

        async function scanWifi() {
            try {
                const result = await api("/api/connection/scan");
                renderSsidList(result.networks || []);
                appendLog(log, { scan: result });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        }

        el("refreshDetails").addEventListener("click", loadDeviceDetails);
        el("connectWifi").addEventListener("click", async () => {
            try {
                const payload = {
                    wifiSsid: el("wifiSsid").value,
                    wifiPassword: el("wifiPassword").value,
                    apEnabled: el("apEnabled") ? el("apEnabled").checked : true
                };
                const result = await api("/api/connection", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(log, { connectWifi: { ssid: payload.wifiSsid, apEnabled: payload.apEnabled }, result: result });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        });
        el("applyDeviceName").addEventListener("click", async () => {
            try {
                const payload = {
                    networkName: el("networkName").value,
                    apPassword: el("apPassword") ? el("apPassword").value : "",
                    apEnabled: el("apEnabled") ? el("apEnabled").checked : true
                };
                const result = await api("/api/connection", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(deviceLog, { applyAP: payload, result: result });
                await loadConnection();
                await loadDeviceDetails();
            } catch (err) {
                appendLog(deviceLog, { error: String(err) });
            }
        });

        el("saveConnection").addEventListener("click", async () => {
            try {
                const payload = {
                    mqttHost: el("mqttHost").value,
                    mqttPort: Number(el("mqttPort").value),
                    mqttUsername: el("mqttUsername").value,
                    mqttPassword: el("mqttPassword").value,
                    mqttBaseTopic: el("mqttBaseTopic").value
                };
                const result = await api("/api/connection", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(log, { applyMqtt: payload, result: result });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        });

        el("applyTopics").addEventListener("click", async () => {
            try {
                const payload = {
                    mqttCommandTopic: el("mqttCommandTopic").value,
                    mqttStateTopic: el("mqttStateTopic").value,
                    mqttEventsTopic: el("mqttEventsTopic").value,
                    mqttWarningsTopic: el("mqttWarningsTopic").value,
                    mqttGameStateTopic: el("mqttGameStateTopic").value,
                    mqttPropStateTopic: el("mqttPropStateTopic").value
                };
                const result = await api("/api/connection", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                appendLog(log, { applyTopics: payload, result: result });
            } catch (err) {
                appendLog(log, { error: String(err) });
            }
        });

        loadConnection();
        loadDeviceDetails();
        scanWifi();
        setInterval(scanWifi, 10000);
        setInterval(loadDeviceDetails, 15000);
    }

    window.PX = {
        pageDashboard,
        pageConfig,
        pageConnection
    };
})();
