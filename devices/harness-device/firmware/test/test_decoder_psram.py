"""W6: the cable_link decoder buffer lives in PSRAM, and nothing can hand it to a DMA engine.

The buffer is a CPU-only parse buffer. It is safe in PSRAM only while that stays true, so this test pins the facts
the move relies on (checked in ESP-IDF 5.5.6 when W6 was written):

  * The RX side: the USB-Serial-JTAG driver fills its own ring buffer in the ISR; cable_xport_usb.c reads from
    that ring into the reader task's 256-byte chunk with usb_serial_jtag_read_bytes, and the decoder copies
    the chunk into its buffer with memcpy (cable_decoder_buf_feed). The driver never sees the decoder buffer.
  * The frame callback gets a pointer into the buffer. The consumers of that pointer are a fixed list, and each
    one copies or parses it with the CPU. The OTA slice goes to esp_ota_write -> esp_flash_write, which copies a
    source that is not in internal DRAM through a stack buffer, 32 bytes at a time
    (components/spi_flash/esp_flash_api.c, direct_write / temp_buf), so it is slower from PSRAM but correct;
    the product build has the OTA path compiled out (DEVICE_NO_OTA) and drops this consumer entirely.
  * Only cable_link.c names the buffer.
If this test fails because a consumer was added, check that the new one does no DMA from the pointer, then add it here.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'
link = (main / 'cable_link.c').read_text().replace('\r\n', '\n')
client = (main / 'cable_client.c').read_text().replace('\r\n', '\n')


def code_lines(text):
    """Source lines with // comments and string literals removed."""
    out = []
    for line in text.splitlines():
        line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
        line = line.split('//')[0]
        out.append(line)
    return out


# 1. the buffer is in PSRAM, the way s_tx_frame already is
assert '#include "esp_attr.h"' in link
assert re.search(r'^static EXT_RAM_BSS_ATTR uint8_t\s+s_dec_buf\[CABLE_MAX_FRAME\];', link, re.M), 'decoder buffer is not EXT_RAM_BSS_ATTR'
assert re.search(r'^static EXT_RAM_BSS_ATTR uint8_t s_tx_frame\[CABLE_MAX_FRAME\];', link, re.M)
defaults = (main / '../sdkconfig.defaults.esp32s3').read_text() + (main / '../sdkconfig.defaults').read_text()
assert re.search(r'^CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y', defaults, re.M), 'PSRAM BSS needs this symbol'

# 2. only cable_link.c names the buffer, and there it appears in exactly the CPU-side places
for path in sorted(main.rglob('*')):
    if path.suffix not in ('.c', '.h') or path.name == 'cable_link.c':
        continue
    text = '\n'.join(code_lines(path.read_text(errors='replace')))
    assert not re.search(r'\bs_dec_buf\b', text), f'{path.name} takes the decoder buffer'
uses = [l.strip() for l in code_lines(link) if re.search(r'\bs_dec_buf\b', l)]
assert len(uses) == 2, uses
assert any(re.match(r'static EXT_RAM_BSS_ATTR uint8_t\s+s_dec_buf\[CABLE_MAX_FRAME\];', u) for u in uses)
assert 'cable_decoder_init_with(&s_decoder, s_dec_buf, sizeof s_dec_buf)' in ' '.join(uses)
# the decoder object is only fed, reset and read for its counters: never passed to a driver or a copy
allowed = re.compile(r'cable_decoder_(?:init_with|buf_feed|buf_reset)\(&s_decoder\b|\bs_decoder\.(?:len|corrupt_frames|discarded_bytes)\b'
                     r'|^static cable_decoder_buf_t\s+s_decoder;')
for line in code_lines(link):
    if re.search(r'\bs_decoder\b', line):
        assert allowed.search(line.strip()), f'unexpected use of the decoder: {line.strip()}'

# 3. the bytes reach the buffer by memcpy from the reader's chunk, never from a peripheral
frame_c = (main / 'cable_frame.c').read_text()
feed = re.search(r'void cable_decoder_buf_feed\(.*?\n\}\n', frame_c, re.S).group(0)
assert 'memcpy(d->buf + d->len, data, take);' in feed
reader = re.search(r'static void reader_task\(void \*arg\)\n\{.*?^\}', link, re.M | re.S).group(0)
assert re.search(r'uint8_t chunk\[READ_CHUNK\];', reader) and 's_xport->read(s_xport, chunk, sizeof(chunk)' in reader
assert 'cable_decoder_buf_feed(&s_decoder, chunk' in reader
usb = (main / 'cable_xport_usb.c').read_text()
assert 'usb_serial_jtag_read_bytes(buf, cap' in usb and 's_decoder' not in usb and 's_dec_buf' not in usb

# 4. the consumers of the frame callback's payload pointer are a fixed list
on_frame = re.search(r'static void on_frame\(.*?\n\}\n', client, re.S).group(0)
callees = set()
for line in code_lines(on_frame):
    line = line.replace('(const char *)', '')
    for call in re.finditer(r'\b([A-Za-z_]\w*)\s*\(([^()]*)\)', line):   # innermost calls: name(args without parentheses)
        if re.search(r'\bpayload\b', call.group(2)) and call.group(1) not in ('if', 'while', 'sizeof', 'on_frame'):
            callees.add(call.group(1))
assert callees == {'fw_update_slice', 'handle_pet_slice', 'cable_json_guard', 'cJSON_ParseWithLengthOpts'}, callees
# each of them uses the pointer on the CPU
fw = (main / 'fw_update.c').read_text()
assert re.search(r'esp_ota_write\(s_handle, data, len\)', fw)
pets = (main / 'pet_store.c').read_text()
assert 'memcpy(s_in->buf + s_in->got, data, len);' in pets
guard = (main / 'cable_json_guard.c').read_text()
assert not re.search(r'\b(?:spi_|i2s_|esp_lcd|gdma|dma_)', guard)

print('decoder buffer in PSRAM: EXT_RAM_BSS_ATTR; named only in cable_link.c; filled by memcpy from the reader chunk; '
      'payload consumers are fw_update_slice, handle_pet_slice, cable_json_guard and cJSON, all CPU PASS')
