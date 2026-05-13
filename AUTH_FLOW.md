# Authentication Flow

## Boundary

Offline and online authentication are separate systems.

Offline auth:

- ESP32 local only
- MAC address registration/authentication
- ESP NVS storage
- active only when local AP/offline web portal is running

Online auth:

- Supabase Auth username/password flow
- Supabase tables and RLS
- GitHub Pages frontend
- active only for online dashboards

No offline credential is accepted online. No online credential is accepted offline.

## Offline Authentication

Offline authentication is implemented in firmware by `WebPortal` and `StorageLayer`.

Storage:

- ESP NVS
- `users_json`
- local password hashes
- MAC address records

User record behavior:

- MAC address identifies the local device.
- Password is validated by the ESP.
- Admin and manager flags are local firmware permissions.
- Restricted users are sent to the restricted/simple local page.

Offline flow:

1. ESP has no usable internet/server path.
2. Firmware enters offline mode.
3. ESP SoftAP and captive portal start.
4. Browser opens local ESP page from LittleFS.
5. `WebPortal` resolves the client MAC address from the AP station list.
6. `/api/auth/status` reports whether that MAC is known.
7. Login validates MAC and password against ESP NVS.
8. Local WebSocket/API commands are accepted only for authorized local users.
9. Relay/timer/PIR commands go to `ControlEngine`.

Offline auth never calls Supabase.

## Online Authentication

Online authentication is implemented by Supabase Auth and the online static frontend.

Storage:

- Supabase Auth users
- `public.smart_home_profiles`
- `public.smart_home_device_memberships`

Login fields:

- username
- password

Online flow:

1. User opens GitHub Pages `online/index.html`.
2. Browser loads `online/config.js`.
3. Browser creates a Supabase client with public URL and public key.
4. Browser calls `auth_email_for_username(username)`.
5. Browser signs in with Supabase Auth using resolved email and password.
6. Browser loads profile and memberships under RLS.
7. Frontend computes role.
8. Admin redirects to `online/admin.html`.
9. Normal user redirects to `online/simple.html`.

Online auth never reads ESP NVS or the offline MAC roster.

## Role Resolution

The frontend treats a session as admin when either condition is true:

- `smart_home_profiles.global_role = 'admin'`
- any loaded membership has `role = 'admin'`

The database uses the same concept in private RLS helper functions:

- `smart_home_private.current_user_is_global_admin()`
- `smart_home_private.is_device_admin(device_id)`
- `smart_home_private.is_device_member(device_id)`

## Admin Behavior

Admin users go to the advanced online page:

- file: `online/admin.html`
- script: `online/app.js`

Admin controls:

- manual ON/OFF/AUTO
- start timer
- cancel timer
- energy tracking toggle
- PIR mapping
- rated power
- consumption reset
- live sensor status
- synced activity events

These commands are queued in Supabase, claimed by the ESP, and executed by `ControlEngine`.

## Normal User Behavior

Normal users go to the simple online page:

- file: `online/simple.html`
- script: `online/app.js`

Normal controls:

- manual ON/OFF/AUTO
- start timer
- cancel timer

Normal users cannot stay on the admin page. If they open it directly, frontend routing sends them back to `simple.html`. RLS also prevents normal users from inserting advanced command payloads manually.

## Command Authorization

Browser command path:

1. Authenticated browser inserts into `smart_home_remote_commands`.
2. RLS verifies `user_id = auth.uid()`.
3. RLS verifies the user is a device member.
4. RLS validates command JSON shape.
5. RLS validates command role:
   - normal users: relay/timer/get-state commands
   - admins: relay/timer/get-state plus advanced commands
6. ESP claims pending commands by token RPC.
7. ESP applies command through `ControlEngine`.
8. ESP writes done/failed result.

The browser never receives or sends `CLOUD_COMMAND_TOKEN`.

## Device Token Authorization

Firmware stores:

```c
CLOUD_COMMAND_TOKEN
```

Supabase stores:

```sql
encode(digest(convert_to(token, 'UTF8'), 'sha256'), 'hex')
```

Token-checked RPC protects:

- state upsert
- event insert
- command claim
- command completion

The token is not an online user password. It is only a device-to-server credential.

## Offline Auth Routes

Offline routes are served only when `WebPortal` is running:

- `/api/auth/status`
- `/api/auth/login`
- `/api/auth/logout`
- `/api/auth/addUser`
- `/api/auth/removeUser`
- `/users`
- local WebSocket command channel

When online mode starts, `WebPortal::end()` stops the server and these routes are not served.

## Online Auth Files

Online frontend files:

- `online/index.html`
- `online/admin.html`
- `online/simple.html`
- `online/app.js`
- `online/config.js`

Online database file:

- `supabase/smart_home_schema.sql`

Online deploy file:

- `.github/workflows/deploy-online.yml`

These files are not flashed to ESP LittleFS.

## Invalid Mixed-Auth Designs

These are not allowed:

- online login using ESP MAC users
- offline login using Supabase Auth
- ESP flash storing Supabase user passwords
- GitHub Pages reading `users_json`
- browser sending the device token
- Supabase directly toggling GPIO pins
- online pages served from the ESP while internet mode is active

## Failure Behavior

### Internet Lost In Online Mode

- firmware records repeated failures
- AP remains off until fallback threshold is reached
- firmware switches to offline mode
- local AP/captive portal return
- offline MAC authentication becomes active

### Supabase Auth Fails

- online login is denied
- no device data is readable without RLS authorization
- ESP local control is unaffected

### Unauthorized Online User

- cannot load device data without membership/global admin role
- cannot insert commands without membership
- cannot insert advanced commands without admin role

### Unauthorized Offline User

- unknown MAC cannot authenticate
- restricted MAC receives restricted local controls
- unauthorized WebSocket clients are rejected
