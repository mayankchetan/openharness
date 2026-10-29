"""HT_DRAWBUF_PSRAM (plan C13 stage a): one of the two panel draw buffers lives in PSRAM and goes to the panel by DMA from there.

The strip loop (display_habitat.c paint) alternates two 22,368 B buffers, and starts every damage list on buffer 0, so a
frame of one strip never touches PSRAM; buffer 1 is the PSRAM one. Measured earlier on the dial (diet2-r1-results.md
section 7): +22,532 B internal RAM free, full-frame max +13 percent (25.1 to 28.4 ms), +27 percent against the same
load without it under a 17.8 MB/s PSRAM copy load. The switch is HT_DRAWBUF_PSRAM, default 1 = one buffer; 0 = both internal.

On the ESP32-S3 the SPI driver sends a PSRAM buffer straight from PSRAM when the IO was created with psram_dma_direct,
for any length and start: the transmit external-memory alignment of the AHB-GDMA v1 channel is 1 byte (IDF 5.5.6
gdma.c gdma_config_transfer: only the RX channel and AHB-GDMA v2 / AXI need one, and spi_master.c setup_dma_priv_buffer
bounces a transfer only when (address | length) & (alignment - 1) is not zero). So the short last strip of a face
(9,320 B) needs no padding and no switch to the internal buffer. The buffer is still cache-line aligned and rounded up
to the line, so the cache write-back (esp_cache_msync) before the DMA read touches nothing but this buffer.

display_habitat.c cannot be compiled on the PC, so this reads the wiring from the source; the allocation itself is
test_draw_buffer.c.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent / '..'
display = (root / 'main/ui/habitat/display_habitat.c').read_text()

assert re.search(r'#ifndef HT_DRAWBUF_PSRAM\s+#define HT_DRAWBUF_PSRAM 1\b', display), 'default is one buffer in PSRAM'
assert '#include "draw_buffer.h"' in display

io = re.search(r'#if HT_DRAWBUF_PSRAM\s*\n([^#]*?)#endif', display[display.index('io_cfg.trans_queue_depth'):], re.S)
assert io and 'io_cfg.flags.psram_dma_direct = 1' in io[1], 'the panel IO sends a PSRAM colour buffer by DMA from PSRAM'

alloc = re.findall(r'draw_buffer_alloc\(([^;]*)\);', display)
assert len(alloc) == 1, alloc
assert 'HT_DRAWBUF_PSRAM' in alloc[0] and 'i == 1' in alloc[0], 'only the second buffer, only when the switch is on'
assert not re.search(r'pixels\[i\]\s*=\s*heap_caps_malloc', display), 'no second path that allocates the buffers'

cmake = (root / 'main/CMakeLists.txt').read_text()
assert '"draw_buffer.c"' in cmake, 'draw_buffer.c is built'
assert re.search(r'PRIV_REQUIRES[^)\n]*\besp_mm\b', cmake), 'esp_mm: esp_private/esp_cache_private.h'
assert re.search(r'foreach\(psram_switch [^)]*\bHT_DRAWBUF_PSRAM\b', cmake), 'cmake passes the switch through'

print('HT_DRAWBUF_PSRAM: buffer 1 only, via draw_buffer_alloc, psram_dma_direct on the panel IO, default on, cmake switch PASS')
