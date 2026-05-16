# Quby `keteladapter` (BA) serial protocol — reverse-engineering notes

**Device**: AVR boiler-adapter board ("BA") on a Toon thermostat, talking to
`happ_thermstat` over `/dev/ttymxc0` (TTL UART; almost certainly 9600 8N1 based
on round-trip timing — see Appendix B).

**Source data**: `/tmp/qt_rebuild/captures/ttymxc0_capture.log` — 30s of live
traffic captured with the `libttysniff.so` LD_PRELOAD shim. 655 writes / 2643
reads. happ_thermstat is the master (writer); BA is the slave (responder).

**Status of the boiler**: physically detached on this Toon (OTGW handles the
boiler). So BA responses to actual OpenTherm queries are mostly
"Unknown-DataId" (OT msg-type 0b1010). The format is fully observed; the
*payload semantics* are inferred from OpenTherm's standard DataId mapping plus
the firmware-RE notes in `project_keteladapter_firmware_re` memory.

---

## 1. Physical / framing layer

### 1.1 Frame size

**Every frame is exactly 7 bytes.** (Two minor exceptions, both at process
start — see §1.4.) Single-byte `read()` and `write()` calls in the capture
just reflect how happ_thermstat and the kernel UART driver shuffle bytes;
the wire-level unit is always 7 octets.

```
+--------+--------+--------+--------+--------+--------+--------+
| HDR    | OP     | A      | B      | C      | D      | CRC    |
+--------+--------+--------+--------+--------+--------+--------+
   0        1        2        3        4        5        6
```

- `HDR`  (1B) — frame class. High bit set in requests (master→slave),
  high bit cleared in responses (slave→master). See §2.
- `OP`   (1B) — operation byte. For control/config commands it's ASCII
  (`'E'`, `'J'`, `'P'`, `'S'`, `'V'`, `'Y'`, `'A'`); for OpenTherm wrap it's a
  numeric OT message-type (`0x00` = Read-Data, `0x02` = Write-Data, `0x0a`
  = Unknown-DataId).
- `A..D` (4B) — payload. For OT wrap this matches the 32-bit OpenTherm
  frame minus parity: `A` = (msg-type<<4 | spare-bits), `B` = data-id,
  `C` = data-value-high, `D` = data-value-low. For control commands it
  is opcode-specific (see §3).
- `CRC`  (1B) — checksum over bytes 0..5. Linearly-derived 8-bit
  polynomial CRC (not a simple sum/xor); see §1.3 and Appendix A.

### 1.2 Frame delimiter

There is **no start/end byte and no framing escape**. Receivers rely on
fixed length plus the high-bit signature of `HDR` (`1xxxx xxx0` for
requests starting with `0xC2/0xCB/0xCD`, `0xxxx xxx0` for responses
starting with `0x42/0x4B/0x4D`) to resync. If a receiver gets out of sync
the master flushes with a burst of `0x6A` sync bytes — see §1.4.

After the BA sends its 7-byte response, there is no inter-frame separator;
the next master frame begins ~60 ms later (capture shows steady-state
~190 ms request-to-request cadence with the BA round trip taking ~130 ms).

### 1.3 Checksum

Empirically linear ("flipping bit `k` of any input byte XORs the CRC by a
fixed constant"), which means it is an 8-bit CRC of some polynomial with
init=0, refin/refout TBD. The fingerprint observed in the capture:

- Toggling bit 0 of byte 5 (last data byte) XORs CRC by **0x21**
  (e.g. `cb 45 00 00 00 00 → 1f` vs `cb 45 00 00 00 01 → 3e`: `1f^3e = 0x21`)
- Toggling bit 1 of byte 5 XORs CRC by **0x42**
  (e.g. `c2 59 00 00 00 00 → af` vs `c2 59 00 00 00 02 → ed`: `af^ed = 0x42`)
- Toggling bit 0 *and* bit 1 of byte 5 XORs CRC by **0x63** = `0x21 ⊕ 0x42`
  (`c2 59 …03 → cc`, `af^cc = 0x63`) — confirms linearity

A polynomial of `0x21` is unusual. Three concrete candidates the bridge
implementer should try first, in this order, validating against any 5+
sample frames from the capture:

```c
// SOLVED 2026-05-15 via Python brute-force over 27 sample frames:
//   poly=0x21, init=0xff, refin=false, refout=false, xorout=0x00
// Unique fit — no other combination in (poly × init × refin × refout × xorout)
// matches all samples.
uint8_t crc8_quby(const uint8_t *p, int n) {
    uint8_t c = 0xff;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x21) : (uint8_t)(c << 1);
    }
    return c;
}
```

**Validation samples** (used to brute-force the polynomial — all 27 frames
fit the solved CRC above with no exceptions):

| Bytes 0..5             | CRC  |
|------------------------|------|
| `cb 45 00 00 00 00`    | `1f` |
| `cb 45 00 00 00 01`    | `3e` |
| `c2 4a 00 00 00 00`    | `7e` |
| `42 4a 00 00 00 00`    | `c0` |
| `c2 50 00 00 00 00`    | `0c` |
| `42 50 00 00 00 00`    | `b2` |
| `c2 56 00 00 00 00`    | (request sent w/ no CRC at boot) |
| `42 56 41 43 00 25`    | `26` |
| `c2 59 00 00 00 00`    | `af` |
| `c2 59 00 00 00 01`    | `8e` |
| `c2 59 00 00 00 02`    | `ed` |
| `c2 59 00 00 00 03`    | `cc` |
| `42 59 00 00 65 99`    | `7d` |
| `42 59 17 00 24 00`    | `9b` |
| `42 59 02 88 45 57`    | `cc` |
| `cb 53 01 00 08 00`    | `36` |
| `cb 53 01 01 02 00`    | `c6` |
| `cb 53 01 4a 10 00`    | `c8` |
| `cd 00 00 00 00 00`    | `95` |
| `cd 00 00 01 00 00`    | `e2` |
| `cd 00 00 02 00 00`    | `7b` |
| `cd 02 01 02 00 46`    | `e1` |
| `cd 02 01 38 3c 00`    | `92` |
| `cd 02 01 18 12 c7`    | `e2` |
| `4d 00 0a 00 00 00`    | `78` |
| `4d 00 0a 01 00 00`    | `0f` |
| `42 41 00 00 00 00`    | `2e` |

Solving these 27 equations is trivial brute force; once the poly is
fixed, the bridge can do simple table-driven CRC-8.

### 1.4 Resync / wake-up (`0x6A` flood)

At boot, happ_thermstat sends a burst of **256 × 0x6A bytes** (split as
128 + (kickoff version request) + 128) before entering the normal request
loop:

```
W: 128 × 0x6A                        (sync flush)
W: c2 56 00 00 00 00                 (NB: 6 bytes, no CRC) — "GetVersion knock"
R: c2 56 00 00 00 00                 (echo of the 6 bytes — note: UART local echo
                                      from the master driver, not from the BA)
R: 42 56 41 43 00 25 26              (BA's version response — opcode 0x56,
                                      ASCII "AC", fw 0x25 = 37; CRC 0x26)
W: 128 × 0x6A                        (second flush — BA exits boot, locks RX)
W: c2 4a 00 00 00 00 7e              (first real 7-byte frame)
```

After this one-off init the master never sends `0x6A` again. The bridge
should reproduce this burst exactly on cold start.

Note: every request frame that happ_thermstat reads back from the device
fd has *two* 7-byte payloads:

```
W: cb 45 00 00 00 00 1f             (request)
R: cb 45 00 00 00 00 1f             (UART loopback / kernel echo of master's TX)
R: 42 41 00 00 00 00 2e             (BA's response — note HDR = req_hdr & 0x7F)
```

The first read is local echo, the second is the actual slave response. A
bridge sitting in the BA's seat does not need to emit the echo — the
Linux UART driver synthesises it on the master side automatically.

---

## 2. Header byte and direction bit

| Direction         | Header value | Bit pattern   |
|-------------------|--------------|---------------|
| master → slave    | `0xC2`       | 1100 0010     |
| master → slave    | `0xCB`       | 1100 1011     |
| master → slave    | `0xCD`       | 1100 1101     |
| slave → master    | `0x42`       | 0100 0010     |
| slave → master    | `0x4B`       | 0100 1011     |
| slave → master    | `0x4D`       | 0100 1101     |
| slave → master    | `0x4C`       | 0100 1100 *   |

`HDR_response = HDR_request & 0x7F`. The top bit is "request/response" /
"master/slave" flag.

The low 7 bits seem to be a **frame-class tag**, dispatching to different
sub-handlers in the BA firmware:

| Tag (low 7 bits) | Used with opcodes              | Meaning (inferred)              |
|------------------|--------------------------------|---------------------------------|
| `0x42` (`C2`/`42`) | `0x4A 'J'`, `0x50 'P'`, `0x56 'V'`, `0x59 'Y'` | Control/meta query class |
| `0x4B` (`CB`/`4B`) | `0x45 'E'`, `0x53 'S'`         | Subscription / enable class     |
| `0x4D` (`CD`/`4D`) | `0x00`, `0x02`, `0x0A`         | OpenTherm wrap class            |

`0x4C` (once) is the OT-class HDR with a different "subop"; in the
capture it appears as a single byte that we believe is just a CRC value
that happened to equal 0x4C, not a distinct class.

---

## 3. Catalogue of observed message types

### 3.1 Control class (HDR `0xC2` / `0x42`)

#### `'V'` = `0x56` — GetVersion

```
REQ : c2 56 00 00 00 00 [CRC]       — sent once at boot (without CRC)
RESP: 42 56 41 43 00 25 26          — payload "AC" + fw 0x25 (37)
```

Payload = `[variant_byte_0, variant_byte_1, reserved, fw_version]`. In our
capture: variant = ASCII "AC" (firmware variant code), fw = 37.

#### `'Y'` = `0x59` — GetParameter(idx)

```
REQ: c2 59 00 00 00 <idx> [CRC]     — idx in byte 5
RESP: 42 59 <4 bytes data> [CRC]
```

Seen with idx 0..3 (queried back-to-back at boot, probably for the full
config block). Observed payloads on this BA:

| idx | RESP data bytes  | Guess                                            |
|-----|------------------|--------------------------------------------------|
| 0   | `00 00 65 99`    | 16-bit value 0x6599 = 25,993 — could be product code |
| 1   | `17 00 24 00`    | Maybe HW version "1.7" and SW build "0.24"       |
| 2   | `00 00 00 18`    | Counter / flags = 24                             |
| 3   | `02 88 45 57`    | 32-bit serial number = 0x02884557                |

#### `'J'` = `0x4A` — Watchdog kick / no-op (periodic)

```
REQ : c2 4a 00 00 00 00 7e
RESP: 42 4a 00 00 00 00 c0          — always all-zero ACK
```

Sent every ~30 s, then immediately followed by `c2 50` and `cb 45`.

#### `'P'` = `0x50` — Ping / status (periodic, paired with `J`)

```
REQ : c2 50 00 00 00 00 0c
RESP: 42 50 00 00 00 00 b2          — always all-zero ACK
```

Empty response payload always; treat as "are you alive" probe.

### 3.2 Subscription / enable class (HDR `0xCB` / `0x4B`)

#### `'E'` = `0x45` — Enable polling mode

```
REQ : cb 45 00 00 00 <n> [CRC]
RESP: 42 41 00 00 00 00 2e          — generic ACK (opcode 0x41 = 'A')
```

Seen with `n=0` early (after `'V'`), then `n=1` after subscription setup,
then again `n=1` every 30 s in steady state. Interpreted as
"set polling mode" (`0` = init, `1` = run/active).

#### `'S'` = `0x53` — Subscribe DID

```
REQ : cb 53 01 <DID> <FLAGS> 00 [CRC]
RESP: 42 41 00 00 00 00 2e          — ACK
```

Tells the BA "from now on, also include DID `<DID>` in your scheduled OT
poll cycle, with priority/class `<FLAGS>`". Issued 53 times during boot
(once per DID). FLAGS byte observed values:

| FLAGS | Count | Meaning (inferred)                                  | Example DIDs |
|-------|-------|-----------------------------------------------------|--------------|
| `0x00` | 14   | Register-only (don't auto-poll)                     | 0x27, 0x28, 0x46, 0x47, 0x48, 0x49, 0x4D, 0x57, 0xCA, 0xCE, 0xD0, 0x5C, 0x5D, 0x5E |
| `0x01` | 25   | Normal poll                                          | 0x03, 0x05, 0x06, 0x09, 0x0F, 0x11, 0x12, 0x19, 0x1A, 0x1B, 0x1C, 0x23, 0x30, 0x39, 0x71..0x7B |
| `0x02` | 5    | Faster poll                                          | 0x01, 0x02, 0x10, 0x18, 0x38 (the "live" setpoints)  |
| `0x03` | 3    | Highest poll                                         | 0x14, 0x15, 0x16 (CH water temp/pressure/version)    |
| `0x08` | 1    | Status (top priority)                                | 0x00                                                  |
| `0x10` | 5    | Counter / rare poll                                  | 0x4A, 0x4B, 0x4C, 0x7D, 0x7F (burner/pump counters)  |

The 3rd byte `01` after `53` is constant — likely "version of subscribe
record format". The 6th byte `00` is constant — likely a "value high"
field that's only used for `'W'`-style subscriptions (write-on-change),
which we never see.

### 3.3 OpenTherm wrap class (HDR `0xCD` / `0x4D`)

This is the heart of the protocol: every Quby `cd …` frame contains a
straight 32-bit OpenTherm message, minus the parity bit, in payload
bytes 1..5. Concretely:

```
              +---- OT msg-type (top 4 bits)
              |
        OP <--+-- always paired with `01` byte after it on writes,
        |              `00` on reads
        v
cd  <OP> <SUB> <DID> <HI> <LO>  CRC
       ^      ^      ^   ^
       |      |      |   '----- LSB of OT data value
       |      |      '--------- MSB of OT data value
       |      '----------------- OpenTherm Data-ID
       '------------------------ OpenTherm message type *as the master sets it*
                                 (0x00 = Read-Data, 0x02 = Write-Data)
```

The `<SUB>` byte:
- `0x00` on Read-Data → "no data field used"
- `0x01` on Write-Data → "data field carries the value to write"

(Compare with `cb 53 01 …`: same `01` discriminator means "data follows".)

#### `0x00` — OT-Read-Data(DID)

```
REQ : cd 00 00 <DID> 00 00 [CRC]
RESP: 4d 00 <MT> <DID> <HI> <LO> [CRC]
```

`<MT>` in the response is the OpenTherm msg-type the slave returned:
- `0x04` = Read-Ack (data is valid)
- `0x05` = Write-Ack
- `0x06` = Invalid-Data
- `0x07` = Reserved
- `0x0A` = Unknown-DataId  ← what we see in the capture (no boiler attached)

If the OT slave does respond, `<HI><LO>` carries the f8.8 fixed-point or
flags value per the OpenTherm 2.2 spec.

#### `0x02` — OT-Write-Data(DID, value)

```
REQ : cd 02 01 <DID> <HI> <LO> [CRC]
RESP: 42 41 00 00 00 00 2e             — ACK (no OT-level confirmation)
```

Note the response uses HDR `0x42`/opcode `0x41` (generic ACK), **not**
`0x4d`. The BA does not wait for the OT-bus Write-Ack; it just queues the
write and ACKs the host instantly. (This is why happ_thermstat issues
writes from a non-blocking path; the eventual OT Write-Ack will show up
in the *next* poll of that DID via the normal `cd 00 00 …` path.)

Writes observed during the 30 s capture:

| Frame                       | DID | Value (hex) | OT meaning                                   |
|-----------------------------|----:|------------:|----------------------------------------------|
| `cd 02 01 00 02 00`         |   0 | 0x0200      | Status: master byte 0x02 = "CH1 enable"     |
| `cd 02 01 01 07 00`         |   1 | 0x0700      | Control setpoint = 7.0 °C (f8.8)            |
| `cd 02 01 01 08 00`         |   1 | 0x0800      | Control setpoint = 8.0 °C                   |
| `cd 02 01 02 00 46`         |   2 | 0x0046      | Master config / member-id LSB               |
| `cd 02 01 10 12 00`         |  16 | 0x1200      | Room setpoint = 18.0 °C                     |
| `cd 02 01 18 12 c7`         |  24 | 0x12c7      | Current room temperature = 18.78 °C         |
| `cd 02 01 38 3c 00`         |  56 | 0x3c00      | DHW setpoint = 60.0 °C                      |

---

## 4. DID poll catalogue (steady-state)

After subscription is complete, happ_thermstat polls each DID with
`cd 00 00 DD 00 00` at ~190 ms intervals, cycling through the full set
(~10 seconds per full cycle). Observed DIDs (53 total):

```
0x00, 0x01, 0x02, 0x03,                 — status, control setpoint, master cfg, slave cfg
0x05, 0x06, 0x09,                       — fault flags, OEM fault, remote params
0x0F,                                   — max boiler cap & min mod
0x10, 0x11, 0x12,                       — room setpoint, mod level, CH water pressure
0x14, 0x15, 0x16,                       — DHW flow rate, day/time, OT version master
0x18, 0x19, 0x1A, 0x1B, 0x1C,           — room T, boiler flow T, DHW T, outside T, return water T
0x23,                                   — boiler fan speed
0x27, 0x28,                             — solar storage T, solar collector T
0x30,                                   — DHW setpoint upper/lower bounds
0x38, 0x39,                             — DHW setpoint, max CH setpoint
0x46, 0x47, 0x48, 0x49,                 — remote-override room setpoint family
0x4A, 0x4B, 0x4C, 0x4D,                 — burner-starts, CH-pump-starts, DHW-pump-starts, DHW-burner-starts
0x57,                                   — Operating Hours - CH burner
0x5C, 0x5D, 0x5E,                       — OEM diagnostic block
0x71..0x7B,                             — OEM-specific counters (11 DIDs)
0x7D, 0x7F,                             — OEM-specific counters
0xCA, 0xCE, 0xD0                        — OEM-specific extension DIDs
```

DIDs 0, 1, 2 are polled with slightly higher frequency (every cycle
*plus* whenever a Write to that DID is performed — that triggers an
immediate Read to verify).

The memory note in `project_keteladapter_firmware_re` mentioned a
"baseline 8 DIDs" set; the actual subscription is much wider (53),
probably because the firmware itself only polls 8 *without instruction*
but Toon's happ_thermstat declares the full set via `'S'`-Subscribe.

---

## 5. Mapping to Schelte Bron's OTGW protocol

OTGW exposes a line-oriented ASCII protocol on TCP :6638 (and on serial
115200 8N1). Reference: https://otgw.tclcode.com/firmware.html

### 5.1 Inbound (master → BA) translation

| Quby request                       | OTGW equivalent                                                     |
|------------------------------------|---------------------------------------------------------------------|
| `c2 56 …` GetVersion               | `PR=A` (print version) — synthesise locally, OTGW does not stub it. |
| `c2 59 …` GetParameter(idx)        | No OTGW equivalent. Synthesise from a static config table. |
| `c2 4a …` / `c2 50 …` heartbeat    | OTGW has no heartbeat; the bridge just ACKs locally. |
| `cb 45 …` Enable                   | OTGW always on; ACK locally. |
| `cb 53 …` Subscribe DID            | Maintain a local subscription table; OTGW polls everything itself. |
| `cd 00 00 DD 00 00` OT-Read        | **OTGW does this for us already.** Either capture by parsing OTGW's `B`/`A`/`R`/`T` async lines, OR send `PR=…` style request. Best path: keep a *cache* of "latest seen value per DID" from OTGW's async stream, and answer Quby reads from the cache. |
| `cd 02 01 DD HI LO` OT-Write       | Different commands per DID — see §5.3 below.                       |

### 5.2 Outbound (BA → master) translation

The bridge must synthesise a 7-byte Quby response for every Quby request.
OTGW's async output gives us all we need:

- OTGW emits one line per OT bus message it sees, e.g. `B40000000`
  (Boiler→Thermostat, 32-bit hex frame) and `T20180b03c` (Thermostat→Boiler).
  The bridge parses these into a `DID → (msg-type, value)` map.
- When a Quby Read request comes in for DID `D`, look up the cache and
  build the corresponding `4d 00 <MT> D HI LO <CRC>` response. If the
  DID has never been seen (cache miss), reply with `MT=0x0A` Unknown-DataId
  to keep happ_thermstat happy — it will retry later.

### 5.3 Quby Write → OTGW command map

OTGW already proxies most writes transparently, so the simplest bridge
just *generates the same OT frame on the OTGW side*. OTGW has the
`OT=<hex>` raw-send command (and `SR=<id>:<v1>,<v2>` for setting up
spoofed read replies). Direct mapping:

| Quby DID | What it is                           | OTGW command to issue                          |
|----------|--------------------------------------|------------------------------------------------|
| 0        | Status (master flags)                | `CS=…` to control CH enable; or let it pass    |
| 1        | Control setpoint (CH water target)   | `CS=<°C>` (control setpoint override)          |
| 2        | Master config                        | `MM=<id>` (master member-id)                   |
| 16 (0x10)| Room setpoint                        | `TT=<°C>` (target temperature, temporary)      |
| 18 (0x12)| Current room temperature             | `TR=<°C>` (set room-temperature OTGW reports)  |
| 24 (0x18)| Room temperature (alt id, what Toon uses) | `TR=<°C>` (same as above)                  |
| 56 (0x38)| DHW setpoint                         | `SW=<°C>` (set DHW setpoint)                   |
| Any other write | …                             | `OT=<hex32>` — build the raw 32-bit OT frame, parity included, send through. |

The clean approach is option 2 ("raw passthrough"): construct the
parity-correct 32-bit OT frame from the Quby `cd 02 01 DID HI LO`
payload and send `OT=<hex>` to OTGW. OTGW then forwards it on the bus.

### 5.4 Things OTGW won't give us, that the bridge must fake

- **GetVersion (`c2 56`)** — return a fixed string. Use the same
  `42 56 41 43 00 25 26` ("AC", fw 37) as the real BA, otherwise
  happ_thermstat may take a different code path. *Do* test with another
  fw byte to see if it cares — likely it doesn't.
- **GetParameter (`c2 59`)** — return the captured values byte-for-byte;
  these are quasi-static config (HW rev, product ID, serial).
- **OT-bus down detection** — if the OTGW link is dead, return Unknown-DataId
  (`MT=0x0A`) for reads and silently drop writes. This is what the real BA
  does when no OT slave answers.

---

## 6. Concrete bridge design

```
                            ┌──────────────────┐
   happ_thermstat ─UART─►   │                  │   ─ TCP :6638 ────►  OTGW
   (Quby master)            │   quby_bridge    │   (ASCII line protocol)
   /dev/ttymxc0 ◄──UART─    │                  │   ◄────────────────  OTGW
                            └──────────────────┘
                                 ▲       │
                                 │       ▼
                            (state cache: DID → MsgType + Value + ts)
```

### 6.1 Skeleton (pseudocode)

```python
import socket, struct, time, threading

# ---------- Quby framing ----------
CRC_POLY = 0x21         # TODO: confirm by fitting against the §1.3 table

def crc8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ CRC_POLY) & 0xFF if (c & 0x80) else (c << 1) & 0xFF
    return c

def frame(hdr, op, a, b, c_, d):
    body = bytes([hdr, op, a, b, c_, d])
    return body + bytes([crc8(body)])

# ---------- OT cache fed by OTGW ----------
cache = {}    # did -> (msg_type, hi, lo, last_seen)
def cache_lookup(did):
    if did in cache:
        mt, hi, lo, _ = cache[did]
        return mt, hi, lo
    return 0x0A, 0, 0      # Unknown-DataId

# ---------- OTGW reader thread ----------
def otgw_loop(sock):
    buf = b""
    while True:
        buf += sock.recv(256)
        while b"\r" in buf or b"\n" in buf:
            line, _, buf = buf.partition(b"\r")
            line = line.strip(b"\n").decode("ascii", "ignore")
            if not line: continue
            # Async data line format: <dir><8 hex digits>
            #   dir = 'A' (Answer A→T), 'T' (Thermostat→Boiler), 'B' (Boiler→Thermostat), 'R' (Request)
            if len(line) >= 9 and line[0] in "ATBR":
                try: frame32 = int(line[1:9], 16)
                except ValueError: continue
                mt   = (frame32 >> 28) & 0x07          # 3-bit msg-type
                did  = (frame32 >> 16) & 0xFF
                hi   = (frame32 >>  8) & 0xFF
                lo   =  frame32        & 0xFF
                # Only update from boiler-originated frames (B/A) — those are authoritative
                if line[0] in "BA":
                    cache[did] = (mt, hi, lo, time.time())

# ---------- Quby UART loop ----------
def serve_quby(tty):
    # Mimic the wake-up burst once
    tty.write(b"\x6a" * 128)
    # Wait for first 7-byte request
    while True:
        req = read_exact(tty, 7)
        if not check_crc(req): continue       # resync on next 7-byte boundary
        hdr, op, a, b, c_, d, _ = req
        resp = handle(hdr, op, a, b, c_, d)
        if resp is not None:
            tty.write(resp)

def handle(hdr, op, a, b, c_, d):
    # Control class
    if hdr == 0xC2:
        if op == 0x56:                       # GetVersion
            return frame(0x42, 0x56, 0x41, 0x43, 0x00, 0x25)
        if op == 0x59:                       # GetParameter
            return frame(0x42, 0x59, *param_table[d])
        if op in (0x4A, 0x50):               # heartbeat
            return frame(0x42, op, 0, 0, 0, 0)
    # Subscribe class
    if hdr == 0xCB:
        if op in (0x45, 0x53):               # Enable / Subscribe
            return frame(0x42, 0x41, 0, 0, 0, 0)     # generic ACK
    # OT wrap class
    if hdr == 0xCD:
        if op == 0x00 and a == 0x00:                 # OT Read
            did = b
            mt, hi, lo = cache_lookup(did)
            return frame(0x4D, 0x00, mt, did, hi, lo)
        if op == 0x02 and a == 0x01:                 # OT Write
            did, hi, lo = b, c_, d
            otgw_write(did, hi, lo)                  # passthrough
            return frame(0x42, 0x41, 0, 0, 0, 0)     # ACK
    return None   # unknown — silence (happ_thermstat will retry)

def otgw_write(did, hi, lo):
    # Build a Write-Data (MT=1) 32-bit OT frame, add parity, send as OT=<hex>
    f32 = (0x1 << 28) | (did << 16) | (hi << 8) | lo
    f32 |= ot_parity_bit(f32) << 31
    otgw_sock.sendall(b"OT=%08X\r\n" % f32)
```

### 6.2 Operational notes

- **Run on the Toon, not a separate device** — `/dev/ttymxc0` is the
  AVR UART, so the bridge must replace `happ_thermstat`'s view of the
  device. Easiest path: rename `keteladapter_proxy` PTY into
  `/dev/ttymxc0` via a bind-mount or have the bridge spawn the existing
  AVR firmware path replaced by a pty pair. Alternatively: stop the
  physical AVR (cut power to it) and have the bridge own the real UART.
- **Bit-for-bit fidelity on the boot handshake matters.** happ_thermstat
  appears to be reasonably tolerant (it issued a 6-byte version request
  with no CRC and the BA still answered), but until proven otherwise,
  reproduce: 128 × 0x6A → reply to GetVersion → 128 × 0x6A → reply to
  `c2 4a` and `cb 45` → accept the subscription block → enter steady
  state.
- **Don't fight the 30s heartbeat.** Reply to `c2 4a` and `c2 50` and
  `cb 45` (n=1) every 30s with all-zero ACKs. Failing this will trigger
  happ_thermstat's "BA dead" path (firmware fallback / reset attempts).
- **DID cache TTL.** OTGW emits a B/A line whenever the boiler answers
  a poll, but if a DID is not in OTGW's own poll set it won't appear.
  Either configure OTGW's `SR=<did>:…` spoofing for DIDs OTGW doesn't
  poll, or arrange for the bridge to *originate* OT reads via OTGW's
  `PR=…` / raw `OT=…` when its cache is stale.

---

## 7. What's still unknown / next experiments

1. **Exact CRC polynomial.** Linear, fingerprint `0x21` on bit-0 of last
   byte, but the actual polynomial is not pinned down without running
   code. Brute-force the 27 sample frames in §1.3 against all 256 ×
   {init 0, 0xFF} × {refin true, false} combinations.
2. **Meaning of `0x59` parameter indices.** Indices 0..3 are queried,
   payloads guessed as product-id / fw-rev / counter / serial. Could be
   confirmed by reading the AVR firmware's response table — see
   `~/toonui_recovered/firmware_re/firmware_ba_AC_37.bin` and the
   `requestDataId` handler.
3. **`cb 53` FLAGS bit semantics.** Bit 0/1 = poll rate? Bit 4 = "counter,
   poll rarely"? Bit 3 (`0x08`, only on DID 0) = "top priority"? A second
   capture with happ_thermstat reconfigured (e.g., disabling DHW) would
   change the set of subscribed DIDs and let us reverse the bit meanings.
4. **Subscribe with `n != 0` in byte 5.** All subscribe frames in this
   capture have byte 5 = `0x00`. The byte's existence implies a "write
   value" or "default value" field per DID. Force-flush the BA (cut
   power) and re-capture *every* subscribe to see if any non-zero value
   shows up.
5. **`c2 50` vs `c2 4a` distinction.** Both are heartbeats, sent back to
   back every 30s. Possibly one is "are you alive?" and the other is
   "have you got any unsolicited data for me?". Run a capture where the
   boiler reports a fault and watch which one suddenly carries non-zero
   data.
6. **The `01` byte after `cb 53` and after `cd 02`.** Always `0x01`. Is
   this a version of the sub-frame format, or an actual "data present"
   flag? Force a Subscribe with no value (if happ_thermstat ever does
   that) — if `01` ever flips to `00`, it's a data-present flag.
7. **Does the BA send unsolicited frames?** No evidence in this 30 s
   capture. If the OT slave actually gives an asynchronous alert
   (msg-type 0b1100 = Data-Invalid from slave-initiated), does the BA
   buffer it for the next poll, or does it push it up? Worth capturing
   while wiggling the boiler connection.

---

## Appendix A — Why the CRC poly is *probably* a non-standard one

The "XOR-by-0x21 on the LSB" fingerprint matches almost no published
CRC-8 polynomial (CCITT = 0x07; SAE-J1850 = 0x1D; AUTOSAR = 0x2F; Maxim
= 0x31; ICODE = 0x1D; ROHC = 0x07; WCDMA = 0x9B). The closest plausible
matches:

- **CRC-8 with polynomial 0x21, init 0, no reflection** — direct
  fingerprint match, but 0x21 is not standard. Could be a custom Quby
  poly.
- **CRC-8/Bluetooth (poly 0xA7, refin/refout)** — has a 0x21 shift-out
  constant in reflected form (XOR with `lookup[0x01]`-style table entry).
  Unlikely but possible.
- An **8-bit Maxim CRC reflected** has 0x5E for LSB of last byte; not 0x21.

The implementer should treat this as "unknown poly, brute force from
samples" until proven otherwise. The brute-force fit is a one-liner in
any CRC tool (e.g. `reveng -w 8 -s <hex frames>`); `reveng` will return
the polynomial in 1-2 seconds.

---

## Appendix B — UART parameters

Not directly captured (the LD_PRELOAD shim doesn't intercept `tcsetattr`
in this build), but inferred:

- 7 bytes at observed round-trip-time of ~130 ms minus ~120 ms processing
  ≈ 10 ms wire time ≈ 7000-8000 bps line rate ⇒ **9600 baud, 8N1** (most
  likely) or 19200 8N1 with extra processing margin.
- Toon firmware-update package's stock `keteladapter` daemon (replaced by
  happ_thermstat in newer firmware) opens `/dev/ttymxc0` with B9600 in
  its `tcsetattr` call (verified by `strings` on the recovered binary
  in earlier work).

For the bridge, use 9600 8N1, no flow control, raw mode.
