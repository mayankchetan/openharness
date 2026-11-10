#include "ble_link.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ble_state.h"
#include "cable_client.h"
#include "cable_link.h"
#include "cable_xport_ble.h"
#include "cable_xport_tls.h"
#include "config_store.h"
#include "device_mac.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
#include "host/util/util.h"
#include "net_hs_class.h"
#include "net_state.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "power_sleep.h"
#include "ram_telemetry.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "task_util.h"
#include "ui/display.h"

static const char *TAG = "ble_link";

#define BLE_INIT_STACK 5120     // internal; nimble_port_init brings the controller up, which may read the PHY data from NVS
#define BLE_LINK_STACK 8192     // PSRAM; the size the TLS handshake ran on over Wi-Fi (high-water mark logged at every link up and down)
#define COC_PSM 0x0080          // spec: psm
#define COC_MTU CABLE_XPORT_BLE_SDU_MAX
#define SDU_BLK 512             // msys block size: the L2CAP MPS is 512 - 8 = 504 (spec: coc_mps_bytes)
#define RX_BLKS 24              // SDU receive buffers, in PSRAM (bench: a 4,032 B SDU takes 8 blocks)
#define TX_BLKS 16              // SDU transmit buffers, in PSRAM
#define TICK_MS 50              // the link task's tick: deadlines, advertising, serve
#define DRAIN_LINES 8           // log lines sent per tick
#define DLE_DELAY_MS 1000       // spec: dle_request_delay_ms
#define DLE_OCTETS 251          // spec: dle_octets
#define DLE_TIME_US 2120        // what the Pixel gave with 251 octets (bench)
#define INIT_WAIT_MS 20000      // how long ble_link_start waits for the host to sync
#define SYNC_WAIT_MS 8000

#ifdef BLE_DEV_ALLOW_UNBONDED
#define DEV_UNBONDED true
#else
#define DEV_UNBONDED false
#endif

void ble_store_config_init(void);   // the NVS-backed bond store of the IDF's nimble host (no header declares it)

// ── state ───────────────────────────────────────────────────────────────────────────────────────────

static ble_t s_ble;
static SemaphoreHandle_t s_lock;               // s_ble and the flags below
static bool s_started;
static ble_pair_window_t s_pair;               // new bonds only inside this window (closed by default); guarded by s_lock
static struct { bool active; uint16_t conn; uint32_t number; uint64_t since_ms; } s_prompt;   // the numeric-comparison offer on the screen and when it went up (30 s at most); guarded by s_lock
static volatile bool s_churn_cut;              // this link was already ended for burning channels
static _Atomic(const cable_xport_t *) s_tls;
static char s_mac[DEVICE_MAC_STR_LEN];
static uint8_t s_own_addr_type;
static SemaphoreHandle_t s_synced;             // given by the host's sync callback
static struct ble_npl_event s_forget_ev;       // Forget, queued on the host task's event queue (forget_run)

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static struct ble_l2cap_chan *volatile s_chan;
static volatile bool s_adv_active;
static uint64_t s_adv_not_before_ms;           // a refused link waits this long before it advertises again (net_state.h refused ladder)
static uint64_t s_conn_ms;                     // uptime at the connect (the DLE request counts from here)
static bool s_dle_done;
static bool s_handshaking;                     // the link task is inside tls->open(): a channel close is posted after the verdict, not before
static bool s_close_pending;
static net_backoff_t s_refused_ladder;
static unsigned s_links;

static os_membuf_t *s_rx_mem, *s_tx_mem;
static struct os_mempool s_rx_mp, s_tx_mp;
static struct os_mbuf_pool s_rx_pool, s_tx_pool;
static uint8_t *s_sdu_buf;                     // one SDU copied out of its mbuf chain (PSRAM)

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void state_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_lock); }

// The only writer of s_ble. Safe from the host task and from the link task.
static void post(ble_event_t event)
{
    state_lock();
    const ble_state_t before = s_ble.state;
    const bool changed = ble_post(&s_ble, event, now_ms());
    const ble_state_t after = s_ble.state;
    state_unlock();
    if (changed) ESP_LOGI(TAG, "state %s -> %s", ble_state_name(before), ble_state_name(after));
}

static ble_state_t current(uint64_t *since)
{
    state_lock();
    const ble_state_t s = s_ble.state;
    if (since) *since = s_ble.since_ms;
    state_unlock();
    return s;
}

ble_state_t ble_link_state(void)
{
    if (!s_started || !s_lock) return BLE_ST_OFF;
    return current(NULL);
}

bool ble_link_restart(void)
{
    if (!s_started) return false;
    post(BLE_EV_START);
    return true;
}

// ── SDU buffers ─────────────────────────────────────────────────────────────────────────────────────

static bool pools_alloc(void)
{
    if (!s_rx_mem) s_rx_mem = heap_caps_malloc(OS_MEMPOOL_SIZE(RX_BLKS, SDU_BLK) * sizeof(os_membuf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_mem) s_tx_mem = heap_caps_malloc(OS_MEMPOOL_SIZE(TX_BLKS, SDU_BLK) * sizeof(os_membuf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sdu_buf) s_sdu_buf = heap_caps_malloc(COC_MTU, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_rx_mem && s_tx_mem && s_sdu_buf;
}

static void pools_init(void)   // after nimble_port_init (the mempool registry exists)
{
    os_mempool_init(&s_rx_mp, RX_BLKS, SDU_BLK, s_rx_mem, "ble_rx");
    os_mbuf_pool_init(&s_rx_pool, &s_rx_mp, SDU_BLK, RX_BLKS);
    os_mempool_init(&s_tx_mp, TX_BLKS, SDU_BLK, s_tx_mem, "ble_tx");
    os_mbuf_pool_init(&s_tx_pool, &s_tx_mp, SDU_BLK, TX_BLKS);
}

static int rx_arm(struct ble_l2cap_chan *chan)
{
    struct os_mbuf *m = os_mbuf_get_pkthdr(&s_rx_pool, 0);
    if (!m) return BLE_HS_ENOMEM;
    const int rc = ble_l2cap_recv_ready(chan, m);
    if (rc == BLE_HS_EBUSY || rc == BLE_HS_EINVAL) os_mbuf_free_chain(m);   // the stack did not take it: ours to free (ENOENT: taken, the channel owns it)
    return rc;
}

// ── the port of the transport (cable_xport_ble.h) ───────────────────────────────────────────────────

// Ownership of the SDU after ble_l2cap_send (read from ble_l2cap_coc.c of IDF 5.5.6 on the bench, task B6): BUSY and BADDATA come back before the stack takes
// it, so the caller frees; every other answer has already given the SDU to the stack, which frees it itself.
static cable_xport_ble_send_t port_send(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (!chan) return BLE_SEND_ERR;
    struct os_mbuf *m = os_mbuf_get_pkthdr(&s_tx_pool, 0);
    if (!m) return BLE_SEND_NOMEM;
    if (os_mbuf_append(m, data, len) != 0) {
        os_mbuf_free_chain(m);
        return BLE_SEND_NOMEM;
    }
    const int rc = ble_l2cap_send(chan, m);
    if (rc == 0) return BLE_SEND_OK;
    if (rc == BLE_HS_ESTALLED) return BLE_SEND_STALLED;
    if (rc == BLE_HS_EBUSY) {
        os_mbuf_free_chain(m);
        return BLE_SEND_BUSY;
    }
    if (rc == BLE_HS_EBADDATA) {
        os_mbuf_free_chain(m);
        return BLE_SEND_ERR;
    }
    return rc == BLE_HS_ENOMEM ? BLE_SEND_NOMEM : BLE_SEND_ERR;
}

static void port_disconnect(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (chan) ble_l2cap_disconnect(chan);
}

static void port_rearm(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (!chan) return;
    if (rx_arm(chan) != 0) {
        // No receive buffer for the next SDU: the phone would stall for ever. End the channel; the phone opens a new one.
        ESP_LOGW(TAG, "no receive buffer: closing the channel");
        ble_l2cap_disconnect(chan);
    }
}

static size_t port_peer_mtu(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    struct ble_l2cap_chan_info info;
    if (!chan || ble_l2cap_get_chan_info(chan, &info) != 0) return 0;
    return info.peer_coc_mtu;
}

static const cable_xport_ble_port_t PORT = {NULL, port_send, port_rearm, port_disconnect, port_peer_mtu};

// ── NimBLE events ───────────────────────────────────────────────────────────────────────────────────

static int coc_event(struct ble_l2cap_event *event, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);

// What the stack says about this connection. A link it cannot find reads as nothing at all (not trusted).
static bool link_sec(uint16_t conn, ble_sec_t *sec)
{
    memset(sec, 0, sizeof *sec);
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return false;
    sec->encrypted = d.sec_state.encrypted;
    sec->bonded = d.sec_state.bonded;
    sec->authenticated = d.sec_state.authenticated;
    sec->key_size = d.sec_state.key_size;
    return true;
}

// The link's security changed: if it now passes the product rule (encrypted, bonded, authenticated, a 16-byte key) it is TRUSTED and counts as encrypted;
// the development build also counts any encrypted link (and trusts nobody, so every abuse bound applies to it).
static void note_security(uint16_t conn)
{
    ble_sec_t sec;
    (void)link_sec(conn, &sec);
    const bool trusted = ble_sec_trusted(&sec);
    if (trusted) {
        state_lock();
        ble_set_trusted(&s_ble, true);
        state_unlock();
    }
    if (trusted || (DEV_UNBONDED && sec.encrypted)) post(BLE_EV_ENCRYPTED);
}

// Does the bond store hold this peer? (Fix B: with the pairing window closed the dial asks only a bonded peer to encrypt; with it open it asks any peer.) A read of the host's own
// copy of the keys it gave away (the same object type ble_link_bond_count counts); it writes nothing. An address the stack has not resolved to an identity
// address reads as "not bonded": that peer then starts encryption itself, as a phone with a stored key does.
static bool peer_bonded(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return false;
    struct ble_store_key_sec key;
    struct ble_store_value_sec val;
    memset(&key, 0, sizeof key);
    memset(&val, 0, sizeof val);
    key.peer_addr = d.peer_id_addr;
    return ble_store_read_our_sec(&key, &val) == 0;
}

// The bond store is a small fixed list (max_bonds). When it is full, or a write would overflow it, the answer is NO: the stack must not delete the oldest bond to
// make room (ble_store_util_status_rr does), because that bond is the real phone and the one asking for room is not.
static int store_status(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    switch (event->event_code) {
    case BLE_STORE_EVENT_OVERFLOW:
    case BLE_STORE_EVENT_FULL:
        ESP_LOGW(TAG, "bond store full: the new bond is refused (nothing is evicted)");
        return BLE_HS_ENOMEM;
    default:
        return BLE_HS_EUNKNOWN;
    }
}

// Bonds follow the pairing window and nothing else: closed, the dial pairs without bonding, so nothing is saved and the channel is refused (no bond).
static void apply_bonding_policy(void)
{
    state_lock();
    const bool open = ble_pair_is_open(&s_pair, now_ms());
    state_unlock();
    ble_hs_cfg.sm_bonding = open ? 1 : 0;
    ble_hs_cfg.sm_our_key_dist = open ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;
    ble_hs_cfg.sm_their_key_dist = open ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;
}

void ble_link_pairing_window(bool open)
{
    if (!s_started) return;
    state_lock();
    if (open) ble_pair_open(&s_pair, now_ms());
    else ble_pair_close(&s_pair);
    state_unlock();
    apply_bonding_policy();
    ESP_LOGI(TAG, "pairing window %s", open ? "open" : "closed");
}

// ── the GATT table (spec sections 1 and 5; B4c) ─────────────────────────────────────────────────────

// The UUIDs are the vector file's, last byte first (test_ble_link_source.py compares them).
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(0x1d, 0x50, 0x99, 0x8e, 0x7c, 0x65, 0x1c, 0x80, 0x70, 0x4d, 0xe6, 0xbb, 0xf0, 0x01, 0x03, 0xe7);
static const ble_uuid128_t UUID_PSM = BLE_UUID128_INIT(0x1d, 0x50, 0x99, 0x8e, 0x7c, 0x65, 0x1c, 0x80, 0x70, 0x4d, 0xe6, 0xbb, 0xf1, 0x01, 0x03, 0xe7);
static const ble_uuid128_t UUID_STATUS = BLE_UUID128_INIT(0x1d, 0x50, 0x99, 0x8e, 0x7c, 0x65, 0x1c, 0x80, 0x70, 0x4d, 0xe6, 0xbb, 0xf2, 0x01, 0x03, 0xe7);
static const ble_uuid128_t UUID_HINT = BLE_UUID128_INIT(0x1d, 0x50, 0x99, 0x8e, 0x7c, 0x65, 0x1c, 0x80, 0x70, 0x4d, 0xe6, 0xbb, 0xf3, 0x01, 0x03, 0xe7);

static _Atomic uint8_t s_relay_status;   // what the phone last wrote: 0 idle
static _Atomic uint8_t s_home_hint;      // 0 until the phone writes a 1
static uint16_t s_status_handle;

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &UUID_PSM.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ},
            {.uuid = &UUID_STATUS.u, .access_cb = chr_access, .val_handle = &s_status_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = &UUID_HINT.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
            {0},
        },
    },
    {0},
};

uint8_t ble_link_relay_status(void) { return atomic_load(&s_relay_status); }
bool ble_link_home_hint(void) { return atomic_load(&s_home_hint) != 0; }

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    const ble_uuid_t *u = ctxt->chr->uuid;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ble_uuid_cmp(u, &UUID_PSM.u) == 0) {
            static const uint8_t psm[2] = {COC_PSM & 0xff, COC_PSM >> 8};   // u16 little endian
            return os_mbuf_append(ctxt->om, psm, sizeof psm) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_cmp(u, &UUID_STATUS.u) == 0) {
            const uint8_t v = atomic_load(&s_relay_status);
            return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    // The ENC flags already need an encrypted link. A write also needs the rule of the channel (bonded, authenticated, a long key): a phone that paired
    // outside the window is encrypted and nothing more, and must not steer the mode switch or the screen. The development build waives it, like the channel.
    ble_sec_t sec;
    (void)link_sec(conn, &sec);
    if (ble_accept_check(&sec, DEV_UNBONDED, false) != BLE_ACCEPT_OK) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t buf[4];
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   // longer than any valid value (and than buf)
    if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) return BLE_ATT_ERR_UNLIKELY;
    uint8_t value = 0;
    if (ble_uuid_cmp(u, &UUID_HINT.u) == 0) {
        const uint8_t err = ble_hint_check(buf, len, &value);
        if (err) return err;
        atomic_store(&s_home_hint, value);
        ESP_LOGI(TAG, "home hint %u", (unsigned)value);
        return 0;
    }
    if (ble_uuid_cmp(u, &UUID_STATUS.u) == 0) {
        const uint8_t err = ble_relay_status_check(buf, len, &value);
        if (err) return err;
        atomic_store(&s_relay_status, value);
        ESP_LOGI(TAG, "relay status %s", ble_relay_status_name(value));
        return 0;
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static void advertise(void)
{
    uint8_t adv[31], rsp[31];
    const size_t adv_n = ble_adv_data(s_mac, adv, sizeof adv), rsp_n = ble_scan_rsp_data(rsp, sizeof rsp);
    if (!adv_n || !rsp_n) {
        ESP_LOGE(TAG, "no advertising data");
        s_adv_not_before_ms = now_ms() + 1000;
        return;
    }
    struct ble_gap_adv_params ap;
    memset(&ap, 0, sizeof ap);
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min = ap.itvl_max = BLE_GAP_ADV_ITVL_MS(50);   // spec: adv_interval_ms
    int rc = ble_gap_adv_set_data(adv, (int)adv_n);
    if (rc == 0) rc = ble_gap_adv_rsp_set_data(rsp, (int)rsp_n);
    if (rc == 0) rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc == 0) {
        s_adv_active = true;
        char name[24];
        if (ble_adv_name(s_mac, name, sizeof name)) ESP_LOGI(TAG, "advertising name=%s", name);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "advertising did not start (rc=%d): trying again in a second", rc);
        s_adv_not_before_ms = now_ms() + 1000;
    } else {
        s_adv_active = true;
    }
}

static void terminate_link(void)
{
    const uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

// ── the number on the screen (B5) ───────────────────────────────────────────────────────────────────

// Ends the offer when it has a verdict: `answer` is 1 Yes, 0 No, -1 none (the link task asks with -1 every tick, so the end of the window is noticed). Yes
// accepts the pairing; No, or a window that has closed, rejects it and ends the link, and closes the window unless phone mode is on with no bond stored (review
// L3: ble_pair_stays_open_after_no; then the window runs to its original end). The pure rules are ble_numcmp_verdict.
static void prompt_settle(int answer)
{
    state_lock();
    if (!s_prompt.active) {
        state_unlock();
        return;
    }
    const ble_pair_verdict_t v = ble_prompt_verdict(&s_pair, s_prompt.since_ms, now_ms(), answer);   // the window's rule, and 30 s at most on the screen
    if (v == BLE_PAIR_WAIT) {
        state_unlock();
        return;
    }
    const uint16_t conn = s_prompt.conn;
    s_prompt.active = false;
    s_prompt.number = 0;
    ble_set_prompting(&s_ble, false);
    state_unlock();
    // Review L3 (2026-11-10): a No used to close the only window until the next replug. With phone mode on and no bond stored the window now stays open to its
    // original end (never extended, never reopened here: no loop), so the phone can try again; with a bond stored (a second phone, opened by [pair]) it closes as
    // before. The bond count is a call into the host, so it is made outside the lock, and only after a rejection.
    bool close_window = v == BLE_PAIR_REJECT;
    if (close_window && ble_pair_stays_open_after_no(s_started, ble_link_bond_count())) {
        close_window = false;
        ESP_LOGI(TAG, "pairing rejected: no bond is stored, so the window stays open to its end");
    }
    if (close_window) {
        state_lock();
        ble_pair_close(&s_pair);
        state_unlock();
        apply_bonding_policy();   // the window just closed: bonding is off from now, not at the next tick
    }
    struct ble_sm_io io;
    memset(&io, 0, sizeof io);
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = v == BLE_PAIR_ACCEPT;
    const int irc = ble_sm_inject_io(conn, &io);
    if (irc == 0) ESP_LOGI(TAG, "pairing %s (rc=%d)", v == BLE_PAIR_ACCEPT ? "accepted" : "rejected", irc);
    else ESP_LOGW(TAG, "pairing %s but the stack did not take the answer (rc=%d)", v == BLE_PAIR_ACCEPT ? "accepted" : "rejected", irc);
    if (v == BLE_PAIR_REJECT) terminate_link();
}

bool ble_link_pair_prompt(uint32_t *number)
{
    if (!s_started) return false;
    state_lock();
    const bool on = s_prompt.active;
    if (on && number) *number = s_prompt.number;
    state_unlock();
    return on;
}

void ble_link_pair_answer(bool yes) { prompt_settle(yes ? 1 : 0); }

bool ble_link_pairing_busy(void)
{
    if (!s_started) return false;
    state_lock();
    const bool busy = ble_pair_is_open(&s_pair, now_ms()) || s_prompt.active;
    state_unlock();
    return busy;
}

uint32_t ble_link_pairing_left_ms(void)
{
    if (!s_started) return 0;
    state_lock();
    const uint32_t left = ble_pair_left_ms(&s_pair, now_ms());
    state_unlock();
    return left;
}

// How many bonds are stored. A read of the host's bond list (RAM; the NVS copy is written by the host task only); the Phone screen asks once a tick while it
// is open. The stack is not asked while it is not running.
unsigned ble_link_bond_count(void)
{
    if (!s_started) return 0;
    int n = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n) == 0 && n > 0 ? (unsigned)n : 0;
}

// Forget, as it runs on the NimBLE HOST task (review M3: the host task owns the bond store, so the wipe, which writes the store's NVS copy, happens there and
// nowhere else; ble_link_forget only posts it): close the window, drop an offer, wipe every bond once, end the link.
static void forget_run(struct ble_npl_event *ev)
{
    (void)ev;
    state_lock();
    ble_pair_close(&s_pair);
    state_unlock();
    apply_bonding_policy();
    prompt_settle(-1);                 // a number on show is rejected: the window is closed
    const int rc = ble_store_clear();  // every bond, once (the store never evicts by itself)
    ESP_LOGI(TAG, "forget: bonds cleared (rc=%d)", rc);
    terminate_link();
}

void ble_link_forget(void)
{
    if (!s_started) return;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_active = false;   // a connection ends advertising either way
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_conn_ms = now_ms();
            s_dle_done = false;
            s_churn_cut = false;
            post(BLE_EV_LINK_UP);
            // The dial asks for encryption itself (spec section 4) and does not wait for the phone, but only while the pairing window is open or for a peer
            // that is already bonded (ble_security_request_wanted): a request to a stranger makes Android raise a consent prompt that nobody can answer. A peer
            // that is not asked is left alone, and the 3 s deadline and the idle cap drop it. Development build: no bond is asked for, so the link counts as
            // encrypted from the start and the 3 s deadline never fires.
            if (ble_encryption_required(DEV_UNBONDED)) {
                const bool bonded_peer = peer_bonded(s_conn);
                state_lock();
                const bool ask = ble_security_request_wanted(&s_pair, now_ms(), bonded_peer);
                state_unlock();
                const int src = ask ? ble_gap_security_initiate(s_conn) : 0;
                if (src != 0) ESP_LOGW(TAG, "security request failed (rc=%d)", src);
                if (!ask) ESP_LOGI(TAG, "no security request: the pairing window is closed and the peer is not bonded");
            } else {
                post(BLE_EV_ENCRYPTED);
            }
            ESP_LOGI(TAG, "central connected (conn %u)", (unsigned)s_conn);
        } else {
            ESP_LOGW(TAG, "connect failed (status %d)", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason 0x%x)", (unsigned)event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_adv_active = false;
        s_chan = NULL;
        cable_xport_ble_channel_down();
        state_lock();
        s_prompt.active = false;   // a lost link takes the offer with it
        s_prompt.number = 0;
        state_unlock();
        atomic_store(&s_relay_status, 0);   // and what the phone wrote: the next phone starts from idle, and a stale hint steers nothing
        atomic_store(&s_home_hint, 0);
        post(BLE_EV_LINK_DOWN);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status != 0) {
            // The encryption failed (the user said No at the phone, a timeout, a bad key): the offer and the deadline hold go with it, and so does the link.
            state_lock();
            s_prompt.active = false;
            s_prompt.number = 0;
            ble_set_prompting(&s_ble, false);
            state_unlock();
            terminate_link();
            return 0;
        }
        // A bond that is not the product's kind (Just Works, or a short key) is deleted now, so two such pairings cannot fill the two slots.
        struct ble_gap_conn_desc d;
        ble_sec_t sec;
        if (link_sec(event->enc_change.conn_handle, &sec) && ble_gap_conn_find(event->enc_change.conn_handle, &d) == 0 && ble_bond_unwanted(&sec)) {
            ble_store_util_delete_peer(&d.peer_id_addr);
            ESP_LOGW(TAG, "bond is not authenticated or not long-keyed (auth=%d key=%u): deleted, link ended", (int)sec.authenticated, (unsigned)sec.key_size);
            terminate_link();
            return 0;
        }
        note_security(event->enc_change.conn_handle);
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // A peer pairs again although a bond exists. Inside the pairing window the user asked for it: the old bond of THAT peer goes and the pairing goes on.
        // Outside it the request is ignored, so a spoofed address cannot delete the real phone's bond.
        state_lock();
        const bool open = ble_pair_is_open(&s_pair, now_ms());
        state_unlock();
        if (open) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        // The numeric comparison: show the number only inside the pairing window (and one at a time); the screen answers through ble_link_pair_answer. Anything
        // else (the dial displays a code, or must type one) is not a pairing this device does: the link is ended.
        const uint16_t c = event->passkey.conn_handle;
        const bool numcmp = event->passkey.params.action == BLE_SM_IOACT_NUMCMP;
        state_lock();
        const bool show = numcmp && !s_prompt.active && ble_numcmp_request(&s_pair, now_ms()) == BLE_NUMCMP_SHOW;
        if (show) {
            s_prompt.active = true;
            s_prompt.conn = c;
            s_prompt.number = event->passkey.params.numcmp;
            s_prompt.since_ms = now_ms();
            ble_set_prompting(&s_ble, true);   // the 3 s encryption deadline waits while the number is on show
        }
        state_unlock();
        if (show) {
            ESP_LOGI(TAG, "pairing: a number is on the screen");
        } else if (numcmp) {
            struct ble_sm_io io;
            memset(&io, 0, sizeof io);
            io.action = BLE_SM_IOACT_NUMCMP;
            io.numcmp_accept = 0;
            (void)ble_sm_inject_io(c, &io);
            ESP_LOGW(TAG, "pairing refused: the window is closed (or an offer is already on show)");
        } else {
            ESP_LOGW(TAG, "pairing refused: not a numeric comparison");
            terminate_link();
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int coc_event(struct ble_l2cap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT: {
        ble_sec_t sec;
        (void)link_sec(event->accept.conn_handle, &sec);
        switch (ble_accept_check(&sec, DEV_UNBONDED, s_chan != NULL)) {
        case BLE_ACCEPT_OK:
            return rx_arm(event->accept.chan);
        case BLE_ACCEPT_REFUSE_ENC:
            ESP_LOGW(TAG, "channel refused: link not authenticated-bonded (enc=%d bonded=%d auth=%d key=%u)", (int)sec.encrypted, (int)sec.bonded,
                     (int)sec.authenticated, (unsigned)sec.key_size);
            return BLE_HS_EENCRYPT;   // reaches the peer as "insufficient encryption" (ble_l2cap_sig.c)
        case BLE_ACCEPT_REFUSE_SECOND:
        default:
            ESP_LOGW(TAG, "channel refused: one is open already");
            return BLE_HS_ENOMEM;
        }
    }
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status == 0) {
            s_chan = event->connect.chan;
            cable_xport_ble_channel_up();
            ESP_LOGI(TAG, "channel up psm=0x%04x mtu=%u", (unsigned)COC_PSM, (unsigned)port_peer_mtu(NULL));
            post(BLE_EV_CHANNEL_OPEN);
        } else {
            ESP_LOGW(TAG, "channel open failed (status %d)", event->connect.status);
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED: {
        s_chan = NULL;
        cable_xport_ble_channel_down();
        state_lock();
        const bool defer = s_handshaking;
        if (defer) s_close_pending = true;
        state_unlock();
        if (!defer) post(BLE_EV_CHANNEL_CLOSED);
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf *sdu = event->receive.sdu_rx;
        const uint16_t len = OS_MBUF_PKTLEN(sdu);
        bool ok = len <= COC_MTU && os_mbuf_copydata(sdu, 0, len, s_sdu_buf) == 0;
        os_mbuf_free_chain(sdu);   // before the transport re-arms: the next receive buffer comes from the same pool
        if (!ok || !cable_xport_ble_on_sdu(s_sdu_buf, len)) {
            ESP_LOGW(TAG, "bad SDU (%u B): closing the channel", (unsigned)len);
            ble_l2cap_disconnect(event->receive.chan);
        }
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        cable_xport_ble_on_tx_unstalled();
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset (reason %d)", reason); }

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &s_own_addr_type) == 0) {
        const int rc = ble_l2cap_create_server(COC_PSM, COC_MTU, coc_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "no L2CAP server (rc=%d)", rc);
        else xSemaphoreGive(s_synced);
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();   // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ── the link task ───────────────────────────────────────────────────────────────────────────────────

static void log_stacks(const char *when)
{
    TaskHandle_t reader = xTaskGetHandle("cable_link");
    ESP_LOGI(TAG, "stacks %s: ble_link free_min=%u B cable_link free_min=%d B", when, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             reader ? (int)uxTaskGetStackHighWaterMark(reader) : -1);
}

// The TLS handshake runs inside open() on this task, once the phone has opened the channel; 5 s at most (cable_xport_tls.c), and the channel is
// closed on any failure. A close that the host task reports meanwhile is held back until the verdict is in, so the refusal run sees the failure first.
static void handshake(const cable_xport_t *tls)
{
    state_lock();
    s_handshaking = true;
    s_close_pending = false;
    state_unlock();
    power_sleep_hold(PP_HOLD_LINK);   // the chip does not light-sleep in the middle of a handshake (empty inline with battery sleep off)
    const int64_t t0_us = esp_timer_get_time();
    const bool opened = tls->open(tls);
    power_sleep_release(PP_HOLD_LINK);
    const int ms = (int)((esp_timer_get_time() - t0_us) / 1000);
    if (opened) {
        post(BLE_EV_TLS_UP);
        s_links++;
        ESP_LOGI(TAG, "link up #%u handshake_ms=%d boot_ms=%d", s_links, ms, (int)(esp_timer_get_time() / 1000));
        log_stacks("at link up");
        ram_telemetry_checkpoint("ble_link_up");
    } else {
        // Only a TLS alert from the daemon is a refusal (net_hs_class.h); three in a row end the link. Anything else is an ordinary failure.
        const int err = cable_xport_tls_last_error(tls);
        const bool alert = net_hs_refused(err);
        state_lock();
        const ble_event_t verdict = ble_handshake_failed(&s_ble, alert, now_ms());
        const unsigned run = s_ble.refusals.refusals;
        state_unlock();
        if (alert) ESP_LOGW(TAG, "handshake answered with an alert (mbedTLS -0x%04X), %u in a row", (unsigned)-err, run);
        else ESP_LOGW(TAG, "handshake failed after %d ms without an alert (mbedTLS -0x%04X)", ms, (unsigned)-err);
        ESP_LOGI(TAG, "handshake verdict: %s", verdict == BLE_EV_TLS_REFUSED ? "key refused" : "failed");
    }
    state_lock();
    s_handshaking = false;
    const bool closed = s_close_pending;
    s_close_pending = false;
    state_unlock();
    if (closed) post(BLE_EV_CHANNEL_CLOSED);
}

static void ble_link_task(void *arg)
{
    (void)arg;
    const cable_xport_t *tls = atomic_load(&s_tls);
    net_backoff_reset(&s_refused_ladder);
    bool tls_open = false;     // the TLS session of the current channel is established and not yet closed
    bool refused_seen = false; // AUTH_FAILED already acted on
    uint64_t up_ms = 0;
    uint32_t seen_wake = display_wake_seq();   // the screen's wake counter: a touch or the PWR button moves it
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        const uint64_t now = now_ms();
        uint64_t since = 0;
        ble_state_t st = current(&since);

        // 1. deadlines. The poll and the state change are ONE step under the lock, and the glue acts only if the state really changed: a link that was
        // encrypted a moment ago is not dropped, and one that arrived as the advertising timer fired is not lost.
        state_lock();
        const ble_state_t before = s_ble.state;
        ble_enc_hold_update(&s_ble, &s_pair, now, s_prompt.active);   // Fix A: a number on show, or an open window for its first 30 s of this link (review M1), holds the 3 s deadline in SECURING
        const ble_event_t due = ble_poll(&s_ble, now);
        const bool changed = (due == BLE_EV_ADV_TIMEOUT || due == BLE_EV_ENC_TIMEOUT || due == BLE_EV_IDLE_TIMEOUT || due == BLE_EV_START) && ble_post(&s_ble, due, now);
        state_unlock();
        if (changed) ESP_LOGI(TAG, "state %s -> %s (deadline)", ble_state_name(before), ble_state_name(current(NULL)));
        if (changed && due == BLE_EV_ADV_TIMEOUT) {
            ble_gap_adv_stop();
            s_adv_active = false;
            ESP_LOGI(TAG, "no phone in %u ms: not found", (unsigned)BLE_ADV_TIMEOUT_MS);
        } else if (changed && due == BLE_EV_ENC_TIMEOUT) {
            ESP_LOGW(TAG, "link not encrypted after %u ms: dropped", (unsigned)BLE_ENC_DEADLINE_MS);
            terminate_link();
        } else if (changed && due == BLE_EV_IDLE_TIMEOUT) {
            // a peer that is not authenticated-bonded holds the one link slot with no channel: drop it, and stay quiet for a while
            ESP_LOGW(TAG, "peer idle for %u ms without a channel: dropped, %u ms cooldown", (unsigned)BLE_IDLE_CAP_MS, (unsigned)BLE_COOLDOWN_MS);
            terminate_link();
            s_adv_not_before_ms = now + BLE_COOLDOWN_MS;
        } else if (changed && due == BLE_EV_START) {
            // "Phone not found" has lasted BLE_RETRY_MS: advertise again (30 s on the air, 120 s off)
            ESP_LOGI(TAG, "no phone for %u ms: advertising again", (unsigned)BLE_RETRY_MS);
        } else if (due == BLE_EV_TLS_FAIL && !tls_open) {
            // open() has its own 5 s deadline, so this is a channel that opened and never got a handshake: close it, and count an ordinary failure
            port_disconnect(NULL);
            state_lock();
            (void)ble_handshake_failed(&s_ble, false, now);
            state_unlock();
        }
        st = current(&since);

        // 1a. a link that opens and loses channels without TLS (the phone's TLS fails, or an attacker plays one): end it, and wait before advertising
        state_lock();
        const bool burn = ble_churn_exceeded(&s_ble) && !s_churn_cut && s_conn != BLE_HS_CONN_HANDLE_NONE;
        if (burn) s_churn_cut = true;
        state_unlock();
        if (burn) {
            ESP_LOGW(TAG, "%u channels without TLS on one link: dropped, %u ms cooldown", (unsigned)BLE_CHURN_LIMIT, (unsigned)BLE_COOLDOWN_MS);
            terminate_link();
            s_adv_not_before_ms = now + BLE_COOLDOWN_MS;
        }

        apply_bonding_policy();   // the pairing window opens and closes the bonding switch
        prompt_settle(-1);        // the window ending under a number on the screen rejects the pairing

        // 1b. a screen wake (a touch, the PWR button) while "not found": advertise again at once
        const uint32_t wake = display_wake_seq();
        if (wake != seen_wake) {
            seen_wake = wake;
            if (st == BLE_ST_NOT_FOUND) {
                post(BLE_EV_START);
                ESP_LOGI(TAG, "screen wake: advertising again");
                st = current(&since);
            }
        }

        // 2. advertising: while ADVERTISING and not on the air (the first time, after a disconnect, after a refused back-off)
        if (st == BLE_ST_ADVERTISING && !s_adv_active && now >= s_adv_not_before_ms) advertise();
        if (st != BLE_ST_ADVERTISING && s_adv_active && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_adv_stop();
            s_adv_active = false;
        }

        // 3. DLE one second after a connect. Never the 2M PHY (finding FB1): there is no PHY call anywhere in this file.
        const uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE && !s_dle_done && now - s_conn_ms >= DLE_DELAY_MS) {
            s_dle_done = true;
            const int rc = ble_gap_set_data_len(conn, DLE_OCTETS, DLE_TIME_US);
            ESP_LOGI(TAG, "data length request rc=%d", rc);
        }

        // 4. the TLS handshake when the channel has opened
        if (st == BLE_ST_CHANNEL_UP && !tls_open) {
            handshake(tls);
            st = current(&since);
            if (st == BLE_ST_TLS_UP) {
                tls_open = true;
                up_ms = now_ms();
                net_backoff_reset(&s_refused_ladder);
            }
        }

        // 5. the link is up: send queued log lines, notice the end
        if (st == BLE_ST_TLS_UP && tls_open) {
            cable_link_drain_logs(DRAIN_LINES);
            if (!tls->is_up(tls)) post(BLE_EV_CHANNEL_CLOSED);
        }
        if (tls_open && st != BLE_ST_TLS_UP) {
            tls->close(tls);   // the session is over: the TLS state goes, and the next channel starts a new handshake
            tls_open = false;
            ESP_LOGI(TAG, "link down after %u ms", (unsigned)(now_ms() - up_ms));
            log_stacks("at link down");
        }

        // 6. a refused key: drop the link, and wait out the refused back-off before advertising again
        if (st == BLE_ST_AUTH_FAILED && !refused_seen) {
            refused_seen = true;
            state_lock();
            const bool trusted = s_ble.trusted;
            state_unlock();
            const uint32_t wait = ble_refused_wait_ms(trusted, &s_refused_ladder, esp_random());   // flat for a peer that is not authenticated-bonded
            s_adv_not_before_ms = now + wait;
            ESP_LOGW(TAG, "key refused: dropping the link, advertising again in %u ms", (unsigned)wait);
            terminate_link();
        }
        if (st != BLE_ST_AUTH_FAILED) refused_seen = false;
    }
}

// ── transport the cable layer sees ──────────────────────────────────────────────────────────────────

static bool x_open(const cable_xport_t *x) { (void)x; return true; }   // ble_link owns the connection; cable_link starts before the first channel
static void x_close(const cable_xport_t *x) { (void)x; }

static int x_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    if (!t || !t->is_up(t)) {
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return -1;
    }
    return t->read(t, buf, cap, wait_ms);
}

static int x_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    return t ? t->write(t, buf, len, wait_ms) : -1;
}

static bool x_is_up(const cable_xport_t *x)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    return t && t->is_up(t);
}

static const cable_xport_t s_xport = {
    .name = "ble",
    .kind = CABLE_XPORT_BLE,
    .open = x_open,
    .close = x_close,
    .read = x_read,
    .write = x_write,
    .is_up = x_is_up,
};

const cable_xport_t *ble_link_xport(void) { return &s_xport; }

// ── start-up ────────────────────────────────────────────────────────────────────────────────────────

static void zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

// Hand-off between ble_link_start() (app_main) and ble_init_task, as in net_link.c: two parties hold `b`, each releases it, the last frees it.
//   WAITING    start() is waiting for ble_init
//   COMMITTED  ble_init passed its point of no return: start() waits for it for as long as it takes
//   ABANDONED  start() timed out first: ble_init finds this at the same point and starts nothing
typedef struct {
    SemaphoreHandle_t done;
    atomic_int phase;
    atomic_int refs;
    bool ok;
} boot_t;
enum { PHASE_WAITING = 0, PHASE_COMMITTED = 1, PHASE_ABANDONED = 2 };

static void boot_release(boot_t *b)
{
    if (atomic_fetch_sub(&b->refs, 1) != 1) return;
    if (b->done) vSemaphoreDelete(b->done);
    free(b);
}

// The internal-stack half of start-up. Ends by deleting itself.
static void ble_init_task(void *arg)
{
    boot_t *b = arg;
    b->ok = false;
    do {
        if (!pools_alloc()) {
            ESP_LOGE(TAG, "no memory for the SDU buffers");
            break;
        }
        if (!cable_xport_ble_init(&PORT)) break;
        const esp_err_t e = nimble_port_init();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed (0x%x)", (unsigned)e);
            break;
        }
        ble_npl_event_init(&s_forget_ev, forget_run, NULL);
        ble_hs_cfg.reset_cb = on_reset;
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.store_status_cb = store_status;   // a full store refuses the new bond; it never evicts an old one
        // Pairing (B5 adds the screen): numeric comparison with a number on the dial, LE Secure Connections. Bonding starts CLOSED: bonds are made only inside the
        // pairing window (ble_link_pairing_window), and the product channel gate wants an authenticated bond.
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
        ble_hs_cfg.sm_bonding = 0;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_pair_init(&s_pair);
        apply_bonding_policy();
        ble_svc_gap_init();
        ble_svc_gatt_init();
        if (ble_gatts_count_cfg(GATT_SVCS) != 0 || ble_gatts_add_svcs(GATT_SVCS) != 0) {
            ESP_LOGE(TAG, "the GATT table was refused");
            break;
        }
        char name[24];
        if (ble_adv_name(s_mac, name, sizeof name)) ble_svc_gap_device_name_set(name);
        ble_store_config_init();
        pools_init();
        nimble_port_freertos_init(host_task);
        if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(SYNC_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "the BLE host did not sync");
            break;
        }
        // The point of no return, decided against ble_link_start's timeout by one compare-and-swap: from here start() waits for us; before it, start() has
        // already told app_main "USB" and we start nothing.
        int expected = PHASE_WAITING;
        if (!atomic_compare_exchange_strong(&b->phase, &expected, PHASE_COMMITTED)) {
            ESP_LOGW(TAG, "start-up was given up on: not starting the link");
            break;
        }
        ble_init(&s_ble);
        s_started = true;
        post(BLE_EV_START);
        // No touch path reaches Settings > Phone > Pair on a real dial, so with no bond stored the window opens by itself, once, when the phone mode starts (it starts
        // only after an unplug: a replug and an unplug make a new window). It is not opened again at each connect; a No leaves it open to its end (review L3).
        // Numeric comparison and the user's Yes on the dial stay required. With a bond stored it stays shut.
        if (ble_pair_auto_open_wanted(s_started, ble_link_bond_count())) {
            ble_link_pairing_window(true);
            ESP_LOGI(TAG, "no bond stored: the pairing window opened by itself (%u ms)", (unsigned)BLE_PAIR_WINDOW_MS);
        }
#if TASK_STACK_PSRAM_NET
        const BaseType_t task_ok = task_create_psram(ble_link_task, "ble_link", BLE_LINK_STACK, NULL, 4, NULL, tskNO_AFFINITY);
#else
        const BaseType_t task_ok = xTaskCreate(ble_link_task, "ble_link", BLE_LINK_STACK, NULL, 4, NULL);
#endif
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "ble_link task create failed: USB mode");
            s_started = false;
            break;
        }
        b->ok = true;
    } while (0);
    ESP_LOGI(TAG, "ble_init done ok=%d stack free_min=%u B", (int)b->ok, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(b->done);
    boot_release(b);
    vTaskDelete(NULL);
}

bool ble_link_start(void)
{
    config_ble_t cfg;
    if (!config_ble_load(&cfg)) {
        zero(&cfg, sizeof cfg);
        ESP_LOGI(TAG, "no phone mode (the phone settings are not whole)");
        return false;
    }
    char identity[DEVICE_MAC_STR_LEN];
    if (!device_mac_str(identity, sizeof identity)) {
        zero(&cfg, sizeof cfg);
        ESP_LOGE(TAG, "no MAC for the TLS identity");
        return false;
    }
    memcpy(s_mac, identity, sizeof s_mac);
    const cable_xport_t *tls = NULL;
    s_lock = xSemaphoreCreateMutex();
    s_synced = xSemaphoreCreateBinary();
    if (s_lock && s_synced) tls = cable_xport_tls_wrap(cable_xport_ble(), identity, cfg.key, CFG_NET_KEY_LEN);   // copies the key
    zero(&cfg, sizeof cfg);
    if (!tls) {
        ESP_LOGE(TAG, "no memory for the TLS wrapper: USB mode");
        return false;
    }
    atomic_store(&s_tls, tls);
#ifdef BLE_DEV_ALLOW_UNBONDED
    ESP_LOGW(TAG, "DEVELOPMENT BUILD BLE_DEV_UNBONDED_IMAGE: an unbonded phone is accepted; TLS-PSK is the only authentication. Do not ship this image.");
#endif
    boot_t *b = calloc(1, sizeof *b);
    if (!b) {
        ESP_LOGE(TAG, "no memory to start the phone mode: USB mode");
        return false;
    }
    b->done = xSemaphoreCreateBinary();
    atomic_init(&b->phase, PHASE_WAITING);
    atomic_init(&b->refs, 2);   // this caller and ble_init_task
    if (!b->done || xTaskCreate(ble_init_task, "ble_init", BLE_INIT_STACK, b, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ble_init task create failed: USB mode");
        atomic_store(&b->refs, 1);   // no second party exists
        boot_release(b);
        return false;
    }
    ESP_LOGI(TAG, "phone mode (wl_mode = 2): starting the BLE host");
    bool finished = xSemaphoreTake(b->done, pdMS_TO_TICKS(INIT_WAIT_MS)) == pdTRUE;
    if (!finished) {
        int expected = PHASE_WAITING;
        if (atomic_compare_exchange_strong(&b->phase, &expected, PHASE_ABANDONED)) {
            // ble_init is still working and has not passed its point of no return: it will see ABANDONED there and start nothing. `b` is its now.
            ESP_LOGE(TAG, "the phone mode did not start in time: USB mode");
            boot_release(b);
            return false;
        }
        // ble_init had already committed: all that is left for it is creating the link task, so waiting for it is bounded
        finished = xSemaphoreTake(b->done, portMAX_DELAY) == pdTRUE;
    }
    const bool ok = finished && b->ok;
    boot_release(b);
    if (!ok) ESP_LOGE(TAG, "the phone mode did not start: USB mode");
    return ok;
}
