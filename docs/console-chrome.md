# Prop console chrome — px-wifi-v1

Visual and information-architecture source of truth for new `px-*-v1` firmware
(see TFD [ESP32-V1-PLAN.md](../../../rooms/tfd/docs/ESP32-V1-PLAN.md) workstream 0b).

Copy this page set, header, tabs, and CSS variables — then swap Live/Config
content for the prop. Do not restyle each device from scratch.

## Pages

| Page | File | Job |
|------|------|-----|
| Live | `index.html` | Operate the prop and see live state |
| Config | `config.html` | Puzzle / device settings (including Debug when present) |
| Monitor | *(optional)* | Deep diagnostics only if Live would be overcrowded. Tab order: Live, Config, **Monitor**, Connect |
| Connect | `connection.html` | Wi-Fi, MQTT, mDNS/identity, link to OTA |
| OTA | `update.html` | Focused firmware upload; reached from Connect, not a primary tab |

Shared chrome on Live / Config / Connect:

- Header: compressed logo + kicker (`Paradox Prop Console`) + page title
- Tabs with the labels above
- Status icons: Wi-Fi, MQTT (when the prop uses it), power/battery if present

## Look

- CSS variables in `styles.css` (`--blue`, `--green`, `--accent`, `--ok`, …)
- **Signal Glass** (2026-09-02): dark navy stage, lighter navy glass panes, green
  accent from the logo family (`#00a651` / `#00c45c`). Active tab, Ready, and
  Start/Resume/Solve use the green CTA gradient.
- Native `<select>` fields use a light fill and dark type (`#07101f` on `#e8f0ff`).
  Windows list popups stay light; light `--ink` on that list is unreadable.
- Comparison mocks (not shipped in firmware pages): `webui/theme-samples.html`
- Subtle radial/linear CSS gradients only
- No webfont CDNs, no large raster chrome besides the logo
- Path-relative assets + `lib_http_proxy` (`X-Forwarded-Prefix`) so Room Controller `/props/<mdns>/` still works

## Debug

Verbose telemetry is a **Config / NVS Debug** switch, default **off**. Extra
dumps belong on Live (or Monitor) over HTTP/WebSocket — not on MQTT unless
debug-MQTT is explicitly on.

## Local UI iteration (no flash)

The firmware already mocks APIs in `webui/app.js`. Serve the folder and skip
the device:

```powershell
# from px-wifi-v1/
.\scripts\serve_webui.ps1
```

Then open:

- http://127.0.0.1:8090/index.html
- http://127.0.0.1:8090/config.html
- http://127.0.0.1:8090/connection.html
- http://127.0.0.1:8090/update.html

`localhost` / `127.0.0.1` enable demo mocks automatically. On a real device,
`?demo=1` / `?demo=0` force mocks on or off without changing NVS.

Loop:

1. Edit `webui/styles.css` (tokens first) and HTML structure
2. Hard-refresh the local pages
3. Check Live, Config, Connect, OTA at desktop and a narrow phone width
4. Only then flash a bench unit to confirm `lib_http_proxy` + asset size
