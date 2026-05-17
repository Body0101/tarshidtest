# Web UI Contract

## Pages

### Online Dashboard (served by server.js or Supabase hosting)

| Route | Page | Auth | Description |
|-------|------|------|-------------|
| `/` or `/index.html` | User Dashboard | Required | Shows assigned ESPs with relay controls |
| `/admin.html` | Admin Dashboard | Admin only | Full system control with config access |

### Offline AP Mode (served by ESP32)

| Route | Page | Auth | Description |
|-------|------|------|-------------|
| `/` or `/index.html` | Relay Control | None | Local relay control via WebSocket |
| `/wifi.html` | Wi-Fi Setup | None | Provision Wi-Fi credentials |
| `/unauthorized.html` | Access Denied | None | Shown if access roster blocks login |

## User Dashboard (online/index.html)

### States

1. **Loading**: Show spinner while fetching devices and states
2. **Authenticated**: Show assigned ESPs with relay cards
3. **Unauthenticated**: Redirect to Supabase Auth login
4. **Error**: Show error message with retry button
5. **Offline**: Show banner "Internet connection lost — using offline AP mode"

### Layout

```
┌─────────────────────────────────┐
│  Header: Tarshid Smart Building  │
│  [User Avatar] [Logout]          │
├─────────────────────────────────┤
│  [ESP 1 - Living Room]           │
│  ├── Relay 0: [ON] [OFF] ● Light│
│  ├── Relay 1: [ON] [OFF] ● Fan  │
│  ├── Relay 2: [ON] [OFF] ● AC   │
│  └── Relay 3: [ON] [OFF] ● ...  │
│  [ESP 2 - Kitchen]              │
│  ├── Relay 0: [ON] [OFF] ● ...  │
│  └── ...                        │
└─────────────────────────────────┘
```

### Action Button Pattern
- Single action button per relay: shows current state
- On hover/click: expands to show ON/OFF options
- Use CSS transitions for smooth expand/collapse

### Navigation
- Tab-based: Dashboard | Rooms (if multiple) | Settings
- Consistent between admin/user views (admin has additional "Admin" tab)

## Admin Dashboard (online/admin.html)

### States

1. **Loading**: Show spinner
2. **Admin Authenticated**: Show admin controls
3. **Non-admin Authenticated**: Redirect to user dashboard with error
4. **Unauthenticated**: Redirect to login

### Layout

```
┌─────────────────────────────────┐
│  Header: Admin Panel             │
│  [User Avatar] [Logout]          │
├─────────────────────────────────┤
│  Tab: Devices | Users | Logs     │
├─────────────────────────────────┤
│  Devices Tab:                    │
│  ┌────────────────────────────┐ │
│  │ Filter/Search bar          │ │
│  ├────────────────────────────┤ │
│  │ ESP: Living Room (ID: xxx) │ │
│  │ [Rename] [Edit Relays]     │ │
│  │ Last seen: 2 min ago       │ │
│  │ Online: ●                  │ │
│  ├────────────────────────────┤ │
│  │ ESP: Kitchen (ID: yyy)     │ │
│  │ [Rename] [Edit Relays]     │ │
│  │ ...                        │ │
│  └────────────────────────────┘ │
└─────────────────────────────────┘
```

### Relay Editor (Modal/Drawer)

```
┌─ Edit Relays: Living Room ─────┐
│  Channel 0: Light    → [Rename]│
│  Channel 1: Fan      → [Rename]│
│  Channel 2: AC       → [Rename]│
│  Channel 3: Window   → [Rename]│
│  [Save] [Cancel]               │
└────────────────────────────────┘
```

## Offline AP Pages (served by ESP32)

### Relay Control (index.html in data/)

Same layout as user dashboard but:
- Connected directly to ESP32 WebSocket (no Supabase)
- No authentication required
- Shows "Offline Mode" indicator
- Relay names shown as "Relay 0", "Relay 1", etc. or cached friendly names

### Wi-Fi Setup (wifi.html)

```
┌─── Wi-Fi Setup ─────────────────┐
│  SSID: [______________]         │
│  Password: [____________]       │
│  ☐ Always connect on boot       │
│  Backup Network (optional):     │
│  SSID: [______________]         │
│  Password: [____________]       │
│  [Connect]                      │
└─────────────────────────────────┘
```

## Real-time Contract

### Online Mode (Supabase Realtime)
- Connect via Supabase JS client: `supabase.channel('states')`
- Subscribe to INSERT on `states` table for assigned device relays
- Apply state changes to UI immediately on receipt

### Offline Mode (ESP32 WebSocket)
- Connect: `ws://192.168.4.1/ws`
- Message format (bidirectional):

**Client → ESP (Toggle Relay):**
```json
{"action":"toggle","channel":0,"state":"ON"}
```

**ESP → Client (State Update):**
```json
{"type":"relay","channel":0,"state":"ON","source":"browser","ts":1715900000}
```

**ESP → Client (System Event):**
```json
{"type":"event","event":"mode.offline","msg":"Offline local AP mode active.","ts":1715900000}
```
