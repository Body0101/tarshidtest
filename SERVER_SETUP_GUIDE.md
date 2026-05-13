# Server Setup Guide

## Scope

This guide configures the ONLINE mode only:

- Supabase Auth
- Supabase database tables
- RLS policies
- token-checked device RPC
- Realtime publication
- GitHub Pages deployment

It does not configure offline ESP MAC accounts. Offline users remain stored only in ESP NVS.

## Supabase Project

Use any Supabase project. The currently configured example project reference used during development is:

- project ref: `vdgxuhemujbttmclbuxc`
- region: `eu-west-1`
- Postgres: `17`

Do not hard-code this reference unless it is your real project.

## Apply Schema

Open Supabase SQL Editor and run:

```sql
-- supabase/smart_home_schema.sql
```

The schema creates:

- `public.smart_home_profiles`
- `public.smart_home_devices`
- `public.smart_home_device_memberships`
- `public.smart_home_device_states`
- `public.smart_home_device_events`
- `public.smart_home_remote_commands`
- `smart_home_private` helper/RPC implementation schema

The `public` tables are exposed to the browser with RLS. Privileged functions live in `smart_home_private` and are called through public compatibility wrappers.

## Security Model

Browser clients use only the Supabase publishable/anon key. All browser-accessible tables have RLS enabled.

Device writes use RPC functions and a device token:

- firmware stores plain token as `CLOUD_COMMAND_TOKEN`
- Supabase stores only SHA-256 hash in `smart_home_devices.command_token_hash`
- browser never sees the token

Do not put a Supabase service-role key in:

- `online/config.js`
- GitHub Pages secrets used by frontend config
- firmware source files
- committed documentation examples

## Device Registration

Generate a strong random token. Store only its hash in Supabase:

```sql
insert into public.smart_home_devices (id, name, command_token_hash)
values (
  'esp32-main',
  'Main ESP32 Smart Home',
  encode(digest(convert_to('REPLACE_WITH_STRONG_DEVICE_TOKEN', 'UTF8'), 'sha256'), 'hex')
)
on conflict (id) do update
set name = excluded.name,
    command_token_hash = excluded.command_token_hash,
    active = true,
    updated_at = now();
```

Build the ESP firmware with:

```powershell
$env:CLOUD_DEVICE_ID="esp32-main"
$env:CLOUD_COMMAND_TOKEN="REPLACE_WITH_STRONG_DEVICE_TOKEN"
```

## Create Online Users

1. Create a Supabase Auth user with email/password.
2. Copy the Auth user UUID.
3. Insert a `smart_home_profiles` row.
4. Insert a `smart_home_device_memberships` row.

Admin example:

```sql
insert into public.smart_home_profiles (id, username, display_name, global_role)
values ('AUTH_USER_UUID', 'admin', 'Admin User', 'admin')
on conflict (id) do update
set username = excluded.username,
    display_name = excluded.display_name,
    global_role = excluded.global_role,
    active = true;

insert into public.smart_home_device_memberships (device_id, user_id, role)
values ('esp32-main', 'AUTH_USER_UUID', 'admin')
on conflict (device_id, user_id) do update
set role = excluded.role;
```

Normal user example:

```sql
insert into public.smart_home_profiles (id, username, display_name, global_role)
values ('AUTH_USER_UUID', 'user1', 'Normal User', 'user')
on conflict (id) do update
set username = excluded.username,
    display_name = excluded.display_name,
    global_role = excluded.global_role,
    active = true;

insert into public.smart_home_device_memberships (device_id, user_id, role)
values ('esp32-main', 'AUTH_USER_UUID', 'user')
on conflict (device_id, user_id) do update
set role = excluded.role;
```

## RLS Policy Summary

### `smart_home_profiles`

- authenticated users can read their own active profile
- global admins can read all smart-home profiles

### `smart_home_devices`

- members can read their assigned devices
- global admins can read all active devices

### `smart_home_device_memberships`

- members can read memberships for their device
- device admins/global admins can manage memberships

### `smart_home_device_states`

- members can read current state
- ESP writes through token-checked RPC only

### `smart_home_device_events`

- members can read synced events
- ESP writes through token-checked RPC only

### `smart_home_remote_commands`

- members can read commands for their device
- members can insert relay/timer commands
- only admins can insert advanced command types
- ESP claims/finishes commands through token-checked RPC only

Admin-only command types:

- `set_energy_tracking`
- `set_pir_mapping`
- `set_rated_power`
- `reset_consumption`

Normal-user command types:

- `set_manual`
- `set_timer`
- `cancel_timer`
- `get_state`

## Public RPC Surface

The firmware and frontend call these public RPC wrappers:

- `auth_email_for_username(p_username text)`
- `device_upsert_state(p_device_id, p_token, p_updated_epoch, p_state)`
- `device_insert_event(p_device_id, p_token, p_event, p_event_ts, p_dedupe_key, p_payload)`
- `device_claim_commands(p_device_id, p_token, p_limit)`
- `device_finish_command(p_device_id, p_token, p_command_id, p_status, p_ok, p_message, p_processed_epoch)`

Privileged implementation functions are in `smart_home_private`.

## Realtime Setup

The schema adds these tables to the `supabase_realtime` publication:

- `public.smart_home_device_states`
- `public.smart_home_device_events`
- `public.smart_home_remote_commands`

The online frontend subscribes to:

- state updates
- event inserts
- command status updates

## GitHub Pages Setup

In the repository settings:

1. Enable GitHub Pages.
2. Choose GitHub Actions as the Pages source.
3. Add repository secrets:

- `SUPABASE_URL`
- `SUPABASE_ANON_KEY`

The workflow `.github/workflows/deploy-online.yml` deploys the `online/` folder on pushes to `main` or `master`.

The workflow-generated `online/config.js` contains only public Supabase values:

```js
window.TARSHID_CONFIG = {
  supabaseUrl: "...",
  supabaseAnonKey: "..."
};
```

## Firmware Build For Online Mode

```powershell
$env:WIFI_STA_SSID="YourWiFi"
$env:WIFI_STA_PASSWORD="YourWiFiPassword"
$env:SUPABASE_URL="https://YOUR_PROJECT_REF.supabase.co"
$env:SUPABASE_ANON_KEY="your-publishable-or-anon-key"
$env:CLOUD_DEVICE_ID="esp32-main"
$env:CLOUD_COMMAND_TOKEN="REPLACE_WITH_STRONG_DEVICE_TOKEN"
platformio run
```

`scripts/inject_cloud_env.py` turns those environment variables into compiler defines. If a required online value is missing, `CloudSyncService` is not configured and the device stays offline/local.

## Validation Queries

Check users and roles:

```sql
select p.username, p.global_role, p.active, m.device_id, m.role
from public.smart_home_profiles p
left join public.smart_home_device_memberships m on m.user_id = p.id
order by p.username;
```

Check device registration:

```sql
select id, name, active, created_at, updated_at
from public.smart_home_devices;
```

Check latest state:

```sql
select device_id, updated_epoch, updated_at, state
from public.smart_home_device_states;
```

Check recent events:

```sql
select device_id, event, event_ts, payload, created_at
from public.smart_home_device_events
order by created_at desc
limit 50;
```

Check command processing:

```sql
select id, device_id, user_id, status, command, result, created_at, updated_at
from public.smart_home_remote_commands
order by created_at desc
limit 50;
```

## Common Problems

### Login says username not found

Confirm `smart_home_profiles.username` exists and the profile is active.

### Login succeeds but no device appears

Confirm `smart_home_device_memberships` has a row for the Auth user UUID and the device is active.

### Commands stay pending

Confirm:

- ESP is in online mode
- firmware `CLOUD_DEVICE_ID` matches `smart_home_devices.id`
- firmware `CLOUD_COMMAND_TOKEN` hashes to `command_token_hash`
- Supabase URL/key were compiled into firmware

### Pages still requests `main.jsx` or `%BASE_URL%favicon.svg`

That means the deployed Pages site is still the old Vite artifact. Run the updated GitHub Actions workflow from `main` or `master`, and make sure Pages source is set to GitHub Actions.
