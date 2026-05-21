# Writing a freetoon integration

This is the developer guide for building a **marketplace integration** for
freetoon-lvgl. If you can write a small program in any language that compiles
to an ARMv7 Linux binary (or a shell/Python script the Toon can run), you can
add a live tile to the UI.

> Want the runnable starting point instead of reading? Clone
> [`freetoon-integrations`](https://github.com/Ierlandfan/freetoon-integrations)
> and copy `examples/hello-solar/` — this doc explains every piece of it.

---

## 1. How an integration works

An integration is a **standalone daemon** that runs next to `toonui` on the
Toon and pushes its data onto the local **BoxTalk** message bus
(`127.0.0.1:1337`) as a named *service*. `toonui` subscribes to that service
and renders the latest values on a home tile.

```
   your daemon ──(BoxTalk notify on serviceId)──▶ hcb_bxtproxy (:1337) ──▶ toonui
        ▲                                                                    │
        └──────────── runs as an inittab respawn row ◀── installer ──────────┘
                                                              binds tile in Settings → Tiles
```

Key consequences of this design:

- **No patching toonui.** You ship a separate binary; toonui discovers it from
  its `manifest.json` at boot. The UI is generic — your manifest decides the
  tile's title, colour, icon, and which published fields show as the big number
  and the subtitle.
- **Language-agnostic.** toonui only sees BoxTalk frames. C is the reference
  (single static binary, no deps), but anything that can open a TCP socket and
  write bytes works.
- **It's a long-lived loop.** The installer wires your binary into `/etc/inittab`
  with `respawn`, so it restarts if it crashes. Your program should loop
  forever and reconnect on failure (don't exit after one reading).

---

## 2. Anatomy of an integration

An integration is a **gzipped tarball** containing at minimum:

```
manifest.json     # metadata + tile mapping (required)
<your binary>     # the daemon named in manifest.binary (required)
README.md         # optional, recommended
```

On install it lands in `/mnt/data/integrations/<id>/` on the Toon.

### manifest.json

```json
{
  "id": "hello-solar",
  "name": "Hello Solar",
  "version": "1.0.0",
  "binary": "hello-solar",
  "args": [],
  "service_id": "solarProduction",
  "tile": {
    "title": "Solar",
    "color": "0xffcc44",
    "icon": "sun",
    "value_field": "power_w",
    "value_unit": "W",
    "subtitle_field": "today_kwh",
    "subtitle_unit": "kWh today"
  },
  "description": "One-line summary shown in the Marketplace.",
  "license": "MIT"
}
```

| Field | Meaning |
|---|---|
| `id` | Unique slug (`a-z0-9-`). Also the install dir name. |
| `binary` | File in the tarball to run. Must be executable. |
| `args` | Optional argv passed to the binary. |
| `service_id` | The BoxTalk service name your daemon publishes on. toonui subscribes to `urn:hcb-hae-com:serviceId:<service_id>`. |
| `tile.title` | Tile header text. |
| `tile.color` | `"0xRRGGBB"` accent colour (string). |
| `tile.icon` | Icon name mapped via toonui's `icons.h` — e.g. `sun`, `drop`, `flame`, `bolt`, `fan`, `leaf`. Unknown names fall back to a generic glyph. |
| `tile.value_field` | XML element in your notify frame to show as the **big number**. |
| `tile.value_unit` | Unit appended to the value (e.g. `W`). |
| `tile.subtitle_field` | Element shown as the **subtitle**. |
| `tile.subtitle_unit` | Unit/suffix for the subtitle. |

---

## 3. The BoxTalk protocol you must speak

BoxTalk is the Toon's internal RPC bus. You need exactly two message types.

### Framing

**Every frame is XML text followed by a single `NUL` (`0x00`) byte.** The hub
buffers until it sees the NUL. Forgetting the terminator = a silent message
that never reaches subscribers — the #1 bug when writing an integration.

### a) Announce (once per connection, then periodically)

```xml
<discovery nts="ssdp:alive" uuid="YOUR-STABLE-UUID"
  type="urn:schemas-hcb-hae-com:device:thirdParty" version="v"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <service type="solarProduction" version="1"/>
</discovery>
```

Re-announce every few minutes (and after reconnecting) so toonui can
re-subscribe across its own restarts. Use a **stable UUID** — generate one once
and keep it (persist under your install dir); don't randomise per run, or
subscribers rebind every cycle.

### b) Notify (your actual data, as often as it changes)

```xml
<notify uuid="YOUR-STABLE-UUID"
  serviceid="urn:hcb-hae-com:serviceId:solarProduction">
  <power_w>1234</power_w>
  <today_kwh>5</today_kwh>
</notify>
```

The element names **must match** your manifest's `value_field` /
`subtitle_field`. toonui caches the last value per service and paints it on the
next refresh. Send numbers or short strings; toonui appends the units from the
manifest.

> **Sanitise your values.** They go into XML — escape `< > &` (or just emit
> plain numbers). Don't emit megabyte payloads; a tile shows one number + one
> subtitle.

---

## 4. Minimal C daemon

This is the heart of `examples/hello-solar/hello-solar.c` — self-contained, no
libraries beyond libc:

```c
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UUID "hello-solar-7f4e1a08"   /* stable, distinctive */
#define SVC  "solarProduction"

/* XML + a single NUL byte per frame. */
static int send_frame(int fd, const char *xml) {
    if (send(fd, xml, strlen(xml), MSG_NOSIGNAL) < 0) return -1;
    char nul = 0;
    return send(fd, &nul, 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static int bxt_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(1337);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    for (;;) {                                  /* reconnect loop */
        int fd = bxt_connect();
        if (fd < 0) { sleep(5); continue; }

        char buf[512];
        snprintf(buf, sizeof buf,
          "<discovery nts=\"ssdp:alive\" uuid=\"%s\" "
          "type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\">"
          "<service type=\"%s\" version=\"1\"/></discovery>", UUID, SVC);
        send_frame(fd, buf);

        int tick = 0;
        for (;;) {
            int power_w = read_my_sensor();      /* <-- your data source */
            int today_kwh = read_my_total();
            snprintf(buf, sizeof buf,
              "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
              "<power_w>%d</power_w><today_kwh>%d</today_kwh></notify>",
              UUID, SVC, power_w, today_kwh);
            if (send_frame(fd, buf) != 0) break;  /* reconnect on error */
            if (++tick % 30 == 0) send_frame(fd, /* re-announce */ buf);
            sleep(10);
        }
        close(fd);
    }
}
```

Replace `read_my_sensor()` with your real source (HTTP API, serial device,
file, GPIO, Domoticz/HA query, …). That's the only part that changes.

---

## 5. Build

The Toon has **no build toolchain** — cross-compile on your machine for
**ARMv7 hardfloat, glibc** and ship the binary. With the Linaro armhf toolchain:

```sh
arm-linux-gnueabihf-gcc -O2 -static-libgcc -o hello-solar hello-solar.c
```

(The template ships a `Makefile` that does this.) Verify the result:

```sh
file hello-solar    # → ELF 32-bit LSB executable, ARM, EABI5 ... dynamically linked
```

Scripts work too: a `#!/bin/sh` or Python file the Toon can run is a valid
`binary` — no cross-compile needed, just speak BoxTalk from the script.

---

## 6. Package & publish

1. Tar it up (files at the **root** of the archive, not in a subfolder):
   ```sh
   tar czf hello-solar.tar.gz manifest.json hello-solar README.md
   ```
2. Open a PR against
   [`freetoon-integrations`](https://github.com/Ierlandfan/freetoon-integrations):
   - add your folder under `examples/<id>/` (source + manifest + tarball),
   - add an entry to `catalog/index.json`:
     ```json
     {
       "id": "hello-solar",
       "name": "Hello Solar",
       "description": "…",
       "author": "you",
       "version": "1.0.0",
       "url": "https://github.com/Ierlandfan/freetoon-integrations/raw/main/examples/hello-solar/hello-solar.tar.gz",
       "manifest_url": "https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/examples/hello-solar/manifest.json",
       "tile": { "title": "Solar", "color": "0xffcc44", "icon": "sun", "service": "solarProduction" }
     }
     ```

Once merged it appears in **Settings → Marketplace** for every freetoon user.

---

## 7. Install, bind & test

- **From the device UI:** Settings → **Marketplace** → tap your integration →
  Install. (Under the hood this runs `scripts/integrations_install.sh <id>`,
  which fetches the tarball into `/mnt/data/integrations/<id>/` and adds an
  `inittab` respawn row.)
- **Bind it to a tile:** Settings → **Tiles** → pick one of the four
  right-column slots (Energy / Family / Vent / Water) → choose your integration.
  Leaving a slot empty keeps its built-in behaviour.
- **Watch it work:** the daemon's stderr goes to
  `/var/volatile/tmp/integration-<id>.log`. toonui logs subscriptions/notifies
  to `/var/volatile/tmp/toonui.log` — grep for your `service_id`.

### Local dev loop (no Marketplace round-trip)

```sh
scp manifest.json yourbinary root@<toon>:/mnt/data/integrations/<id>/
ssh root@<toon> 'pkill -x toonui'          # toonui re-scans manifests on boot
```
or run the binary by hand and tail the log to see your frames land.

---

## 8. Gotchas checklist

- [ ] **NUL-terminate every frame.** No NUL = silent.
- [ ] **Loop forever + reconnect.** It runs under init `respawn`; a one-shot
      program just respawn-storms.
- [ ] **Stable UUID**, persisted — not random per run.
- [ ] **Element names == manifest fields** (`value_field` / `subtitle_field`).
- [ ] **Re-announce** periodically so toonui re-subscribes after its restarts.
- [ ] **ARMv7 hardfloat** binary (or a script), not x86.
- [ ] Keep payloads tiny and XML-safe.

Questions / review: open an issue or PR on either repo — contributions welcome.
