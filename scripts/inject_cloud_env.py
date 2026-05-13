import os

Import("env")


def _truthy(value):
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def _string_define(name, value):
    escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
    env.Append(CPPDEFINES=[(name, '\\"{}\\"'.format(escaped))])


explicit_enabled = os.environ.get("CLOUD_SYNC_ENABLED")
supabase_url = os.environ.get("SUPABASE_URL") or os.environ.get("VITE_SUPABASE_URL") or ""
supabase_key = (
    os.environ.get("SUPABASE_PUBLISHABLE_KEY")
    or os.environ.get("SUPABASE_ANON_KEY")
    or os.environ.get("VITE_SUPABASE_PUBLISHABLE_KEY")
    or ""
)

cloud_enabled = _truthy(explicit_enabled) if explicit_enabled is not None else bool(supabase_url and supabase_key)
env.Append(CPPDEFINES=[("CLOUD_SYNC_ENABLED", 1 if cloud_enabled else 0)])

if cloud_enabled:
    _string_define("SUPABASE_URL", supabase_url)
    _string_define("SUPABASE_PUBLISHABLE_KEY", supabase_key)
    device_id = os.environ.get("CLOUD_DEVICE_ID", "")
    if device_id:
        _string_define("CLOUD_DEVICE_ID", device_id)
    command_token = os.environ.get("CLOUD_COMMAND_TOKEN") or os.environ.get("SUPABASE_COMMAND_TOKEN") or ""
    if command_token:
        _string_define("CLOUD_COMMAND_TOKEN", command_token)

# ONLINE mode is opt-in at build time. Leaving these blank keeps the existing
# offline/AP firmware behavior exactly as before.
sta_ssid = os.environ.get("WIFI_STA_SSID") or os.environ.get("STA_SSID") or ""
sta_password = os.environ.get("WIFI_STA_PASSWORD") or os.environ.get("STA_PASSWORD") or ""
if sta_ssid:
    _string_define("WIFI_STA_SSID", sta_ssid)
    _string_define("WIFI_STA_PASSWORD", sta_password)
