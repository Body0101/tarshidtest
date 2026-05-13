-- Production Supabase schema for Tarshid Smart Home ONLINE mode.
-- OFFLINE mode remains ESP-local and does not use any table below.

create extension if not exists pgcrypto;

create schema if not exists smart_home_private;
revoke all on schema smart_home_private from public;

create table if not exists public.smart_home_profiles (
  id uuid primary key references auth.users(id) on delete cascade,
  username text not null check (username ~ '^[A-Za-z0-9_.-]{3,32}$'),
  display_name text not null default '',
  global_role text not null default 'user' check (global_role in ('admin', 'user')),
  active boolean not null default true,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create unique index if not exists smart_home_profiles_username_unique
  on public.smart_home_profiles (lower(username));

create table if not exists public.smart_home_devices (
  id text primary key check (id ~ '^[A-Za-z0-9_.:-]{3,64}$'),
  name text not null default 'ESP32 Smart Home',
  command_token_hash text not null check (command_token_hash ~ '^[0-9a-f]{64}$'),
  active boolean not null default true,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.smart_home_device_memberships (
  device_id text not null references public.smart_home_devices(id) on delete cascade,
  user_id uuid not null references auth.users(id) on delete cascade,
  role text not null default 'user' check (role in ('admin', 'user')),
  created_at timestamptz not null default now(),
  primary key (device_id, user_id)
);

create index if not exists idx_smart_home_device_memberships_user
  on public.smart_home_device_memberships (user_id, device_id);

create table if not exists public.smart_home_device_states (
  device_id text primary key references public.smart_home_devices(id) on delete cascade,
  updated_epoch bigint,
  state jsonb not null default '{}'::jsonb,
  updated_at timestamptz not null default now()
);

create table if not exists public.smart_home_device_events (
  id bigserial primary key,
  device_id text not null references public.smart_home_devices(id) on delete cascade,
  event text not null,
  event_ts bigint,
  dedupe_key text unique,
  payload jsonb not null,
  created_at timestamptz not null default now()
);

create index if not exists idx_smart_home_device_events_device_created
  on public.smart_home_device_events (device_id, created_at desc);

create table if not exists public.smart_home_remote_commands (
  id uuid primary key default gen_random_uuid(),
  device_id text not null references public.smart_home_devices(id) on delete cascade,
  user_id uuid not null default auth.uid() references auth.users(id) on delete cascade,
  status text not null default 'pending' check (status in ('pending', 'processing', 'done', 'failed')),
  command jsonb not null,
  result jsonb,
  processed_epoch bigint,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create index if not exists idx_smart_home_remote_commands_pending
  on public.smart_home_remote_commands (device_id, status, created_at asc);
create index if not exists idx_smart_home_remote_commands_user_created
  on public.smart_home_remote_commands (user_id, created_at desc);

alter table public.smart_home_device_states replica identity full;
alter table public.smart_home_device_events replica identity full;
alter table public.smart_home_remote_commands replica identity full;

create or replace function smart_home_private.touch_updated_at()
returns trigger
language plpgsql
set search_path = public, pg_temp
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

drop trigger if exists trg_smart_home_profiles_touch on public.smart_home_profiles;
create trigger trg_smart_home_profiles_touch
before update on public.smart_home_profiles
for each row execute function smart_home_private.touch_updated_at();

drop trigger if exists trg_smart_home_devices_touch on public.smart_home_devices;
create trigger trg_smart_home_devices_touch
before update on public.smart_home_devices
for each row execute function smart_home_private.touch_updated_at();

drop trigger if exists trg_smart_home_device_states_touch on public.smart_home_device_states;
create trigger trg_smart_home_device_states_touch
before update on public.smart_home_device_states
for each row execute function smart_home_private.touch_updated_at();

drop trigger if exists trg_smart_home_remote_commands_touch on public.smart_home_remote_commands;
create trigger trg_smart_home_remote_commands_touch
before update on public.smart_home_remote_commands
for each row execute function smart_home_private.touch_updated_at();

drop function if exists public.touch_updated_at();

create or replace function smart_home_private.current_user_is_global_admin()
returns boolean
language sql
stable
security definer
set search_path = public, auth, pg_temp
as $$
  select exists (
    select 1
    from public.smart_home_profiles p
    where p.id = auth.uid()
      and p.active = true
      and p.global_role = 'admin'
  );
$$;

create or replace function smart_home_private.is_device_member(p_device_id text)
returns boolean
language sql
stable
security definer
set search_path = public, auth, smart_home_private, pg_temp
as $$
  select smart_home_private.current_user_is_global_admin() or exists (
    select 1
    from public.smart_home_device_memberships m
    join public.smart_home_profiles p on p.id = m.user_id
    join public.smart_home_devices d on d.id = m.device_id
    where m.device_id = p_device_id
      and m.user_id = auth.uid()
      and p.active = true
      and d.active = true
  );
$$;

create or replace function smart_home_private.is_device_admin(p_device_id text)
returns boolean
language sql
stable
security definer
set search_path = public, auth, smart_home_private, pg_temp
as $$
  select smart_home_private.current_user_is_global_admin() or exists (
    select 1
    from public.smart_home_device_memberships m
    join public.smart_home_profiles p on p.id = m.user_id
    join public.smart_home_devices d on d.id = m.device_id
    where m.device_id = p_device_id
      and m.user_id = auth.uid()
      and m.role = 'admin'
      and p.active = true
      and d.active = true
  );
$$;

create or replace function smart_home_private.command_payload_shape_allowed(p_command jsonb)
returns boolean
language sql
immutable
set search_path = public, pg_temp
as $$
  with keys as (
    select coalesce(array_agg(key order by key), array[]::text[]) as actual_keys
    from jsonb_object_keys(
      case
        when jsonb_typeof(p_command) = 'object' then p_command
        else '{}'::jsonb
      end
    ) as key
  ), typed as (
    select p_command->>'type' as command_type,
           (select actual_keys from keys) as actual_keys
  )
  select coalesce(case
    when p_command is null or jsonb_typeof(p_command) <> 'object' then false
    when command_type = 'set_manual' then
      actual_keys <@ array['channel','mode','type']::text[] and
      actual_keys @> array['channel','mode','type']::text[] and
      (p_command->>'mode') in ('ON', 'OFF', 'AUTO') and
      (p_command->>'channel') ~ '^[0-9]+$'
    when command_type = 'set_timer' then
      actual_keys <@ array['channel','durationMinutes','durationSec','epoch','target','type']::text[] and
      actual_keys @> array['channel','target','type']::text[] and
      (p_command->>'target') in ('ON', 'OFF') and
      (p_command->>'channel') ~ '^[0-9]+$' and
      (
        ((p_command ? 'durationMinutes') and (p_command->>'durationMinutes') ~ '^[0-9]+$') or
        ((p_command ? 'durationSec') and (p_command->>'durationSec') ~ '^[0-9]+$')
      )
    when command_type = 'cancel_timer' then
      actual_keys <@ array['channel','type']::text[] and
      actual_keys @> array['channel','type']::text[] and
      (p_command->>'channel') ~ '^[0-9]+$'
    when command_type = 'set_energy_tracking' then
      actual_keys <@ array['enabled','type']::text[] and
      actual_keys @> array['enabled','type']::text[] and
      jsonb_typeof(p_command->'enabled') = 'boolean'
    when command_type = 'set_pir_mapping' then
      actual_keys <@ array['mappings','type']::text[] and
      actual_keys @> array['mappings','type']::text[] and
      jsonb_typeof(p_command->'mappings') = 'array'
    when command_type = 'set_rated_power' then
      actual_keys <@ array['channel','powerW','type']::text[] and
      actual_keys @> array['channel','powerW','type']::text[] and
      (p_command->>'channel') ~ '^[0-9]+$' and
      (p_command->>'powerW') ~ '^[0-9]+(\\.[0-9]+)?$'
    when command_type = 'reset_consumption' then
      actual_keys = array['type']::text[]
    when command_type = 'get_state' then
      actual_keys = array['type']::text[]
    else false
  end, false)
  from typed;
$$;

create or replace function smart_home_private.command_payload_allowed_for_user(
  p_device_id text,
  p_command jsonb
)
returns boolean
language sql
stable
security definer
set search_path = public, auth, smart_home_private, pg_temp
as $$
  with typed as (
    select p_command->>'type' as command_type
  )
  select smart_home_private.command_payload_shape_allowed(p_command)
    and (
      command_type in ('set_manual', 'set_timer', 'cancel_timer', 'get_state')
      or (
        command_type in ('set_energy_tracking', 'set_pir_mapping', 'set_rated_power', 'reset_consumption')
        and smart_home_private.is_device_admin(p_device_id)
      )
    )
  from typed;
$$;

create or replace function smart_home_private.auth_email_for_username(p_username text)
returns text
language sql
stable
security definer
set search_path = public, auth, pg_temp
as $$
  select u.email::text
  from auth.users u
  join public.smart_home_profiles p on p.id = u.id
  where lower(p.username) = lower(trim(p_username))
    and p.active = true
  limit 1;
$$;

create or replace function smart_home_private.device_token_ok(p_device_id text, p_token text)
returns boolean
language sql
stable
security definer
set search_path = public, pg_temp
as $$
  select exists (
    select 1
    from public.smart_home_devices d
    where d.id = p_device_id
      and d.active = true
      and d.command_token_hash = encode(digest(convert_to(coalesce(p_token, ''), 'UTF8'), 'sha256'), 'hex')
  );
$$;

create or replace function smart_home_private.device_upsert_state(
  p_device_id text,
  p_token text,
  p_updated_epoch bigint,
  p_state jsonb
)
returns void
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
begin
  if not smart_home_private.device_token_ok(p_device_id, p_token) then
    raise exception 'unauthorized device state sync' using errcode = '42501';
  end if;

  insert into public.smart_home_device_states (device_id, updated_epoch, state, updated_at)
  values (p_device_id, p_updated_epoch, coalesce(p_state, '{}'::jsonb), now())
  on conflict (device_id) do update
    set updated_epoch = excluded.updated_epoch,
        state = excluded.state,
        updated_at = now();
end;
$$;

create or replace function smart_home_private.device_insert_event(
  p_device_id text,
  p_token text,
  p_event text,
  p_event_ts bigint,
  p_dedupe_key text,
  p_payload jsonb
)
returns void
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
begin
  if not smart_home_private.device_token_ok(p_device_id, p_token) then
    raise exception 'unauthorized device event sync' using errcode = '42501';
  end if;

  insert into public.smart_home_device_events (device_id, event, event_ts, dedupe_key, payload)
  values (p_device_id, coalesce(nullif(p_event, ''), 'unknown'), p_event_ts, p_dedupe_key, coalesce(p_payload, '{}'::jsonb))
  on conflict (dedupe_key) do nothing;
end;
$$;

create or replace function smart_home_private.device_claim_commands(
  p_device_id text,
  p_token text,
  p_limit integer default 2
)
returns table (id uuid, command jsonb)
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
begin
  if not smart_home_private.device_token_ok(p_device_id, p_token) then
    raise exception 'unauthorized device command claim' using errcode = '42501';
  end if;

  return query
  with picked as (
    select rc.id
    from public.smart_home_remote_commands rc
    where rc.device_id = p_device_id
      and rc.status = 'pending'
    order by rc.created_at asc
    limit least(greatest(coalesce(p_limit, 2), 1), 10)
    for update skip locked
  )
  update public.smart_home_remote_commands rc
     set status = 'processing',
         updated_at = now()
    from picked
   where rc.id = picked.id
   returning rc.id, rc.command;
end;
$$;

create or replace function smart_home_private.device_finish_command(
  p_device_id text,
  p_token text,
  p_command_id uuid,
  p_status text,
  p_ok boolean,
  p_message text,
  p_processed_epoch bigint
)
returns void
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
begin
  if p_status not in ('done', 'failed') then
    raise exception 'invalid command status' using errcode = '22023';
  end if;
  if not smart_home_private.device_token_ok(p_device_id, p_token) then
    raise exception 'unauthorized device command update' using errcode = '42501';
  end if;

  update public.smart_home_remote_commands rc
     set status = p_status,
         processed_epoch = p_processed_epoch,
         result = jsonb_build_object('ok', coalesce(p_ok, false), 'msg', coalesce(p_message, '')),
         updated_at = now()
   where rc.id = p_command_id
     and rc.device_id = p_device_id
     and rc.status in ('pending', 'processing');
end;
$$;

create or replace function public.current_user_is_global_admin()
returns boolean
language sql
stable
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.current_user_is_global_admin();
$$;

create or replace function public.is_device_member(p_device_id text)
returns boolean
language sql
stable
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.is_device_member(p_device_id);
$$;

create or replace function public.is_device_admin(p_device_id text)
returns boolean
language sql
stable
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.is_device_admin(p_device_id);
$$;

create or replace function public.command_payload_allowed(p_command jsonb)
returns boolean
language sql
immutable
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.command_payload_shape_allowed(p_command);
$$;

create or replace function public.auth_email_for_username(p_username text)
returns text
language sql
stable
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.auth_email_for_username(p_username);
$$;

create or replace function public.device_upsert_state(
  p_device_id text,
  p_token text,
  p_updated_epoch bigint,
  p_state jsonb
)
returns void
language sql
volatile
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.device_upsert_state(p_device_id, p_token, p_updated_epoch, p_state);
$$;

create or replace function public.device_insert_event(
  p_device_id text,
  p_token text,
  p_event text,
  p_event_ts bigint,
  p_dedupe_key text,
  p_payload jsonb
)
returns void
language sql
volatile
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.device_insert_event(p_device_id, p_token, p_event, p_event_ts, p_dedupe_key, p_payload);
$$;

create or replace function public.device_claim_commands(
  p_device_id text,
  p_token text,
  p_limit integer default 2
)
returns table (id uuid, command jsonb)
language sql
volatile
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select * from smart_home_private.device_claim_commands(p_device_id, p_token, p_limit);
$$;

create or replace function public.device_finish_command(
  p_device_id text,
  p_token text,
  p_command_id uuid,
  p_status text,
  p_ok boolean,
  p_message text,
  p_processed_epoch bigint
)
returns void
language sql
volatile
security invoker
set search_path = public, smart_home_private, pg_temp
as $$
  select smart_home_private.device_finish_command(p_device_id, p_token, p_command_id, p_status, p_ok, p_message, p_processed_epoch);
$$;

drop function if exists public.device_token_ok(text, text);

alter table public.smart_home_profiles enable row level security;
alter table public.smart_home_devices enable row level security;
alter table public.smart_home_device_memberships enable row level security;
alter table public.smart_home_device_states enable row level security;
alter table public.smart_home_device_events enable row level security;
alter table public.smart_home_remote_commands enable row level security;

-- Profiles: users can read themselves; global admins can read everyone.
drop policy if exists smart_home_profiles_select on public.smart_home_profiles;
create policy smart_home_profiles_select on public.smart_home_profiles
for select to authenticated
using (id = auth.uid() or smart_home_private.current_user_is_global_admin());

drop policy if exists smart_home_profiles_update_self on public.smart_home_profiles;
create policy smart_home_profiles_update_self on public.smart_home_profiles
for update to authenticated
using (id = auth.uid())
with check (id = auth.uid() and active = true);

-- Devices and memberships are visible only to members/global admins.
drop policy if exists smart_home_devices_select_member on public.smart_home_devices;
create policy smart_home_devices_select_member on public.smart_home_devices
for select to authenticated
using (smart_home_private.is_device_member(id));

drop policy if exists memberships_select_member on public.smart_home_device_memberships;
create policy memberships_select_member on public.smart_home_device_memberships
for select to authenticated
using (smart_home_private.is_device_member(device_id));

drop policy if exists memberships_admin_insert on public.smart_home_device_memberships;
create policy memberships_admin_insert on public.smart_home_device_memberships
for insert to authenticated
with check (smart_home_private.current_user_is_global_admin() or smart_home_private.is_device_admin(device_id));

drop policy if exists memberships_admin_update on public.smart_home_device_memberships;
create policy memberships_admin_update on public.smart_home_device_memberships
for update to authenticated
using (smart_home_private.current_user_is_global_admin() or smart_home_private.is_device_admin(device_id))
with check (smart_home_private.current_user_is_global_admin() or smart_home_private.is_device_admin(device_id));

drop policy if exists memberships_admin_delete on public.smart_home_device_memberships;
create policy memberships_admin_delete on public.smart_home_device_memberships
for delete to authenticated
using (smart_home_private.current_user_is_global_admin() or smart_home_private.is_device_admin(device_id));

-- State/events are read by online users only. ESP writes via token-checked RPC.
drop policy if exists smart_home_device_states_select_member on public.smart_home_device_states;
create policy smart_home_device_states_select_member on public.smart_home_device_states
for select to authenticated
using (smart_home_private.is_device_member(device_id));

drop policy if exists smart_home_device_events_select_member on public.smart_home_device_events;
create policy smart_home_device_events_select_member on public.smart_home_device_events
for select to authenticated
using (smart_home_private.is_device_member(device_id));

-- Users create commands through RLS. ESP claims/finishes through token-checked RPC.
drop policy if exists smart_home_remote_commands_select_member on public.smart_home_remote_commands;
create policy smart_home_remote_commands_select_member on public.smart_home_remote_commands
for select to authenticated
using (smart_home_private.is_device_member(device_id));

drop policy if exists smart_home_remote_commands_insert_member on public.smart_home_remote_commands;
create policy smart_home_remote_commands_insert_member on public.smart_home_remote_commands
for insert to authenticated
with check (
  user_id = auth.uid()
  and status = 'pending'
  and smart_home_private.is_device_member(device_id)
  and smart_home_private.command_payload_allowed_for_user(device_id, command)
);

revoke all on all functions in schema smart_home_private from public;

revoke execute on function public.current_user_is_global_admin() from public;
revoke execute on function public.is_device_member(text) from public;
revoke execute on function public.is_device_admin(text) from public;
revoke execute on function public.command_payload_allowed(jsonb) from public;
revoke execute on function public.auth_email_for_username(text) from public;
revoke execute on function public.device_upsert_state(text, text, bigint, jsonb) from public;
revoke execute on function public.device_insert_event(text, text, text, bigint, text, jsonb) from public;
revoke execute on function public.device_claim_commands(text, text, integer) from public;
revoke execute on function public.device_finish_command(text, text, uuid, text, boolean, text, bigint) from public;

grant usage on schema public to anon, authenticated;
grant select on public.smart_home_profiles to authenticated;
grant select on public.smart_home_devices to authenticated;
grant select, insert, update, delete on public.smart_home_device_memberships to authenticated;
grant select on public.smart_home_device_states to authenticated;
grant select on public.smart_home_device_events to authenticated;
grant select, insert on public.smart_home_remote_commands to authenticated;
grant usage, select on sequence public.smart_home_device_events_id_seq to anon, authenticated;

grant usage on schema smart_home_private to anon, authenticated;
grant execute on function smart_home_private.auth_email_for_username(text) to anon, authenticated;
grant execute on function smart_home_private.device_upsert_state(text, text, bigint, jsonb) to anon;
grant execute on function smart_home_private.device_insert_event(text, text, text, bigint, text, jsonb) to anon;
grant execute on function smart_home_private.device_claim_commands(text, text, integer) to anon;
grant execute on function smart_home_private.device_finish_command(text, text, uuid, text, boolean, text, bigint) to anon;
grant execute on function smart_home_private.current_user_is_global_admin() to authenticated;
grant execute on function smart_home_private.is_device_member(text) to authenticated;
grant execute on function smart_home_private.is_device_admin(text) to authenticated;
grant execute on function smart_home_private.command_payload_shape_allowed(jsonb) to authenticated;
grant execute on function smart_home_private.command_payload_allowed_for_user(text, jsonb) to authenticated;

grant execute on function public.auth_email_for_username(text) to anon, authenticated;
grant execute on function public.device_upsert_state(text, text, bigint, jsonb) to anon;
grant execute on function public.device_insert_event(text, text, text, bigint, text, jsonb) to anon;
grant execute on function public.device_claim_commands(text, text, integer) to anon;
grant execute on function public.device_finish_command(text, text, uuid, text, boolean, text, bigint) to anon;
grant execute on function public.current_user_is_global_admin() to authenticated;
grant execute on function public.is_device_member(text) to authenticated;
grant execute on function public.is_device_admin(text) to authenticated;
grant execute on function public.command_payload_allowed(jsonb) to authenticated;

do $$
begin
  if not exists (
    select 1 from pg_publication_tables
    where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'smart_home_device_states'
  ) then
    alter publication supabase_realtime add table public.smart_home_device_states;
  end if;
  if not exists (
    select 1 from pg_publication_tables
    where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'smart_home_device_events'
  ) then
    alter publication supabase_realtime add table public.smart_home_device_events;
  end if;
  if not exists (
    select 1 from pg_publication_tables
    where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'smart_home_remote_commands'
  ) then
    alter publication supabase_realtime add table public.smart_home_remote_commands;
  end if;
end $$;
