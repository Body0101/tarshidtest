-- V2 Migration: Admin Configuration Management
-- Adds relay_details support and admin config update functions

-- Add relay_details column to smart_home_devices for storing relay-friendly names
alter table public.smart_home_devices
  add column if not exists relay_details jsonb not null default '[]'::jsonb;

comment on column public.smart_home_devices.relay_details is
  'Array of relay configs: [{"channel":0,"friendly_name":"Living Room Light","rated_power_watts":60},...]';

-- Add device_config_updated_at for ESP to know when to re-fetch config
alter table public.smart_home_devices
  add column if not exists config_version integer not null default 0;

-- RPC: admin_update_device_config
-- Updates device name and/or relay details. Requires admin privileges.
create or replace function public.admin_update_device_config(
  p_device_id text,
  p_name text default null,
  p_relay_details jsonb default null
)
returns json
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
declare
  v_updated_at timestamptz;
begin
  -- Verify caller is a global admin or device admin
  if not smart_home_private.current_user_is_global_admin()
     and not smart_home_private.is_device_admin(p_device_id) then
    raise exception 'permission denied: admin role required' using errcode = '42501';
  end if;

  update public.smart_home_devices d
     set name = coalesce(nullif(trim(p_name), ''), d.name),
         relay_details = coalesce(p_relay_details, d.relay_details),
         config_version = d.config_version + 1,
         updated_at = now()
   where d.id = p_device_id
     and d.active = true
  returning d.updated_at into v_updated_at;

  if not found then
    raise exception 'device not found or inactive' using errcode = 'P0002';
  end if;

  return json_build_object('ok', true, 'device_id', p_device_id, 'updated_at', v_updated_at, 'config_version', (select config_version from public.smart_home_devices where id = p_device_id));
end;
$$;

revoke all on function public.admin_update_device_config(text, text, jsonb) from public;
grant execute on function public.admin_update_device_config(text, text, jsonb) to authenticated;

-- RPC: device_fetch_config
-- Returns current config for an ESP device (including relay friendly names).
-- Callable by the device itself via command token.
create or replace function public.device_fetch_config(
  p_device_id text,
  p_token text
)
returns json
language plpgsql
security definer
set search_path = public, smart_home_private, pg_temp
as $$
declare
  v_result json;
begin
  if not smart_home_private.device_token_ok(p_device_id, p_token) then
    raise exception 'unauthorized device config fetch' using errcode = '42501';
  end if;

  select json_build_object(
    'device_id', d.id,
    'name', d.name,
    'relay_details', coalesce(d.relay_details, '[]'::jsonb),
    'config_version', d.config_version,
    'updated_at', d.updated_at
  ) into v_result
  from public.smart_home_devices d
  where d.id = p_device_id and d.active = true;

  return v_result;
end;
$$;

revoke all on function public.device_fetch_config(text, text) from public;
grant execute on function public.device_fetch_config(text, text) to anon;
