"""W13: source rules for the power hooks, checked without a toolchain.

  1. Hook 1, net_power_profile(): it calls esp_wifi_set_ps() and nothing else of the driver, in particular nothing that sets listen_interval.
     WIFI_PS_NONE is named nowhere. Today both profiles are WIFI_PS_MIN_MODEM (default IDLE = ACTIVE until the battery plan sets values).
  2. listen_interval is assigned once, in the association config (wifi_config_t, handed over by esp_wifi_set_config in nwifi_start), and
     named nowhere else in the firmware: it is part of the association request, so no later hook can change it (review finding 8).
  3. PM locks: the net code creates and takes none of its own. While idle (the serve loop, the backoff waits) it holds nothing. Over the connect
     and the handshake it holds PP_HOLD_LINK, the power policy's hold for "a transport with traffic", and lets it go on every path before it waits
     or serves. With battery sleep off (the default image) the hold calls are empty inline functions.
  4. Hook 2, welcome.keepaliveMs on the dial: the hello says keepalive "v1", the welcome's field sets the silence window from the one rule in
     cable_keepalive.h, the session forgets it when it ends, and with no field the numbers are today's (15 s hello, 15 s silence). The daemon's
     twin (cli/src/cable/keepalive.ts) carries the same limits.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'
cli = Path(__file__).resolve().parent / '../../../../cli/src/cable'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    """Source with comments and string literals blanked, so a rule about calls is not fooled by prose."""
    src = text(name)
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)


def body(src, name):
    """The braces of function `name` (a definition at the start of a line), as text."""
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\s*\{', src, re.M)
    assert match, f'no definition of {name}'
    depth, i = 0, match.end() - 1
    start = i
    while True:
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1


def stripped(rel):
    """Code with comments (and, for a .c file, strings) removed; a header keeps its strings."""
    if rel.endswith('.c'):
        return code(rel)
    return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', ' ', text(rel), flags=re.S))


def firmware_sources():
    for path in sorted(main.rglob('*')):
        if path.suffix in ('.c', '.h') and 'managed_components' not in path.parts:
            yield path.relative_to(main).as_posix()


# ── 1. Hook 1 ───────────────────────────────────────────────────────────────────────────────────────
sta = code('wifi_sta.c')
hdr = re.sub(r'//[^\n]*', '', text('wifi_sta.h'))
hook = body(sta, 'net_power_profile')
assert re.search(r'\besp_wifi_set_ps\(', hook), 'the hook calls esp_wifi_set_ps'
calls = set(re.findall(r'\b(esp_wifi_\w+|esp_netif_\w+|esp_event_\w+|nwifi_\w+)\s*\(', hook))
assert calls == {'esp_wifi_set_ps'}, f'the hook calls only esp_wifi_set_ps, not {calls}'
for banned in ('listen_interval', 'wifi_config_t', 'wc.sta', 'esp_wifi_set_config', 'esp_wifi_connect', 'esp_wifi_disconnect', 'esp_wifi_start',
               'esp_wifi_stop', 'WIFI_PS_NONE'):
    assert banned not in hook, f'net_power_profile names {banned}'
assert 'esp_wifi_set_ps(' in sta and len(re.findall(r'\besp_wifi_set_ps\(', sta)) == 1, 'esp_wifi_set_ps is called in one place: the hook'
for rel in firmware_sources():
    if rel != 'wifi_sta.c':
        assert 'esp_wifi_set_ps' not in stripped(rel), f'{rel} sets the power save mode'
assert 'WIFI_PS_NONE' not in sta and 'WIFI_PS_NONE' not in hdr, 'power save is never switched off'
# The two profiles. Today both are min-modem (every number in the spec was measured with it); an IDLE of max-modem is the battery plan's to set.
active = re.search(r'#define\s+NET_PS_ACTIVE\s+(\w+)', sta)
idle = re.search(r'#define\s+NET_PS_IDLE\s+(\w+)', sta)
assert active and active.group(1) == 'WIFI_PS_MIN_MODEM', 'ACTIVE is min-modem'
assert idle and idle.group(1) in ('WIFI_PS_MIN_MODEM', 'WIFI_PS_MAX_MODEM'), 'IDLE is a modem-sleep mode'
assert 'NET_PS_ACTIVE' in hook and 'NET_PS_IDLE' in hook, 'each profile maps to its mode through the two macros'
assert re.search(r'typedef enum\s*\{\s*NET_POWER_ACTIVE\s*,\s*NET_POWER_IDLE\s*\}\s*net_power_t;', hdr), 'two profiles in the header'
assert re.search(r'\bbool\s+net_power_profile\(net_power_t\s+\w+\);', hdr), 'declared in wifi_sta.h'
# an unknown profile is refused before any driver call
assert hook.index('return false') < hook.index('esp_wifi_set_ps('), 'a bad profile is refused first'
# nwifi_start sets ACTIVE through the hook, after the driver is running (esp_wifi_set_ps needs a started driver)
start_sta = body(sta, 'nwifi_start')
assert 'esp_wifi_set_ps' not in start_sta, 'the bring-up goes through the hook'
assert start_sta.index('esp_wifi_start()') < start_sta.index('net_power_profile(NET_POWER_ACTIVE)')

# ── 2. listen_interval ──────────────────────────────────────────────────────────────────────────────
for rel in firmware_sources():
    src = stripped(rel)
    if rel != 'wifi_sta.c':
        assert 'listen_interval' not in src, f'{rel} names listen_interval'
assigns = re.findall(r'listen_interval\s*=[^=]', sta)
assert len(assigns) == 1, 'listen_interval is assigned once'
assoc = start_sta[start_sta.index('memset(&wc, 0, sizeof wc)'):start_sta.index('esp_wifi_set_config(WIFI_IF_STA, &wc)')]
assert 'wc.sta.listen_interval = 3;' in assoc, 'and it is in the association config, before it is handed to the driver'
assert len(re.findall(r'\besp_wifi_set_config\(', sta)) == 1, 'the association config is set in one place'
for name in ('nwifi_connect', 'nwifi_disconnect', 'nwifi_log_ap', 'nwifi_bits', 'nwifi_wait', 'on_event', 'teardown', 'net_power_profile'):
    assert 'listen_interval' not in body(sta, name), name

# ── 3. PM locks ─────────────────────────────────────────────────────────────────────────────────────
NET_FILES = ('net_link.c', 'net_link.h', 'wifi_sta.c', 'wifi_sta.h', 'cable_xport_tcp.c', 'cable_xport_tls.c', 'net_state.c', 'net_state.h')
for rel in NET_FILES:
    assert 'esp_pm_' not in stripped(rel), f'{rel} names a power-management lock of its own'
net = code('net_link.c')
task = body(net, 'net_link_task')
assert len(re.findall(r'power_sleep_hold\(PP_HOLD_LINK\)', net)) == 1 and len(re.findall(r'power_sleep_release\(PP_HOLD_LINK\)', net)) == 1, \
    'one hold and one release in net_link.c'
h, r = task.index('power_sleep_hold(PP_HOLD_LINK)'), task.index('power_sleep_release(PP_HOLD_LINK)')
assert h < task.index('join_wifi()') < task.index('tls->open(tls)') < r, 'the hold covers the join and the handshake'
assert not re.search(r'\b(continue|return|break|goto)\b', task[h:r]), 'no way out between the hold and the release'
assert r < task.index('backoff(') and r < task.index('serve(tls)'), 'released before any backoff wait and before the serve loop'
for name in ('serve', 'backoff', 'sleep_ms', 'join_wifi', 'note_open_failure'):
    assert 'power_sleep_' not in body(net, name), f'{name} takes or releases no hold of its own'
assert '#include "power_sleep.h"' in text('net_link.c')
sleep_h = text('power_sleep.h')
assert re.search(r'static inline void power_sleep_hold\(uint32_t bits\) \{ \(void\)bits; \}', sleep_h), 'an empty inline when battery sleep is off'
assert re.search(r'static inline void power_sleep_release\(uint32_t bits\) \{ \(void\)bits; \}', sleep_h)

# ── 4. Hook 2: welcome.keepaliveMs on the dial ──────────────────────────────────────────────────────
kh = re.sub(r'//[^\n]*', '', text('cable_keepalive.h'))
limits = {k: int(v) for k, v in re.findall(r'#define\s+(KEEPALIVE_(?:MIN|MAX)_MS|KEEPALIVE_SILENCE_FACTOR|KEEPALIVE_DAEMON_EXTRA_MS)\s+(\d+)u', kh)}
assert limits == {'KEEPALIVE_MIN_MS': 5000, 'KEEPALIVE_MAX_MS': 60000, 'KEEPALIVE_SILENCE_FACTOR': 3, 'KEEPALIVE_DAEMON_EXTRA_MS': 5000}, limits
ts = (cli / 'keepalive.ts').read_text(encoding='utf-8')
twin = {
    'KEEPALIVE_MIN_MS': int(re.search(r'KEEPALIVE_MIN_MS = ([\d_]+)', ts).group(1).replace('_', '')),
    'KEEPALIVE_MAX_MS': int(re.search(r'KEEPALIVE_MAX_MS = ([\d_]+)', ts).group(1).replace('_', '')),
    'KEEPALIVE_SILENCE_FACTOR': int(re.search(r'SILENCE_FACTOR = (\d+)', ts).group(1)),
    'KEEPALIVE_DAEMON_EXTRA_MS': int(re.search(r'DAEMON_EXTRA_MS = ([\d_]+)', ts).group(1).replace('_', '')),
}
assert twin == limits, f'the daemon and the dial disagree on the limits: {twin} vs {limits}'

client = code('cable_client.c')
raw_client = text('cable_client.c')
assert '#include "cable_keepalive.h"' in raw_client
assert re.search(r'#define HELLO_SESSION_MS 15000\b', raw_client) and re.search(r'#define HELLO_ALONE_MS\s+2000\b', raw_client), 'the greeting cadence is untouched'
assert 'SILENCE_MS' not in client, 'no fixed silence window is left'
assert re.search(r'static atomic_uint s_keepalive_ms = KEEPALIVE_DEFAULT_MS;', client), 'defaults to today\'s 5 s (3 x 5 s = the old 15 s)'
# strings kept below: the checks name JSON keys and a reason
tick = body(raw_client, 'session_tick')
assert re.search(r'> \(int64_t\)keepalive_dial_silence_ms\(atomic_load\(&s_keepalive_ms\)\) \* 1000\) \{\s*session_down\("silence"\);', tick), 'the dial\'s window is 3 x K'
up = body(raw_client, 'session_up')
assert 'cJSON_GetObjectItemCaseSensitive(p, "keepaliveMs")' in up, 'the welcome field'
assert re.search(r'keepalive_effective_ms\(cJSON_IsNumber\(ka\) \? ka->valuedouble : \(double\)NAN\)', up), 'a field that is not a number is the default'
assert re.search(r'atomic_exchange\(&s_keepalive_ms, ka_ms\)', up), 'and the welcome sets K, on every welcome'
down = body(raw_client, 'session_down')
assert re.search(r'atomic_store\(&s_keepalive_ms, KEEPALIVE_DEFAULT_MS\)', down), 'the session forgets K when it ends'
hello = body(raw_client, 'send_hello')
assert 'msg_string(&root, "keepalive", "v1");' in hello, 'the hello says it reads keepaliveMs'
# only these two places read or write the window
assert len(re.findall(r's_keepalive_ms', client)) == 4, 'definition, the silence check, the welcome, the end of the session'

print('power hooks: net_power_profile (set_ps only, no NONE), listen_interval once in the association config, no PM lock of its own, '
      'hold over connect + handshake only, keepaliveMs wiring and limits agree with the daemon')
