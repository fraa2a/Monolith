# Report: Durata clip doppia del previsto (2×)

## Problema

La durata delle clip salvate dal replay buffer è sempre il **doppio** della durata configurata, indipendentemente da FPS e impostazioni del buffer. L'audio ha il timing corretto ma i fotogrammi video si fermano a metà della clip, e il video scorre al doppio della velocità.

**Esempio:** buffer 30s → clip di 60s, di cui solo i primi 30s hanno video (l'audio va avanti per tutti i 60s).

Il problema si verifica su **AMD** (RX 6600 XT, encoder `hevc_amf`) e su **NVIDIA** (encoder `h264_nvenc`) — quindi non è specifico di un vendor ma comune a tutti gli encoder HW via FFmpeg.

---

## Analisi del pipeline

### 1. OBS Studio (architettura di riferimento)

OBS usa un sistema preciso a 3 livelli:

| Livello | Timebase | Ruolo |
|---------|----------|-------|
| **Video output (`fps_num/fps_den`)** | Configurato dall'utente | Clock master per tutti i timestamp |
| **Encoder layer** | `encoder->timebase_den = voi->fps_num` | Riceve frame con timestamp in `fps_num` |
| **Output / Replay** | `pkt->timebase_den = fps_num` | `dts_usec = dts * 1'000'000 / fps_num` |

OBS calcola `dts_usec` **sempre** usando `fps_num` dal video output, ignorando qualsiasi timebase interno dell'encoder. Inoltre OBS scrive un wall-clock `sys_dts_usec` come safety net.

### 2. Monolith (pre-fix)

Prima del fix, `drain_video()` usava `ctx->time_base` dell'encoder dopo `avcodec_open2`:

```cpp
// OLD (pre-commit a0e255a)
ep.dts_usec = av_rescale(pkt->dts, 1'000'000, ctx->time_base.den);
ep.tb_num   = ctx->time_base.num;
ep.tb_den   = ctx->time_base.den;
```

**Ipotesi originale:** il timebase poteva cambiare dopo `avcodec_open2`, causando discrepanze.

### 3. Monolith (OBS-style fix, commit `a0e255a`)

Abbiamo allineato il codice a OBS: usiamo `cfg_fps` (il valore configurato dall'utente) per tutti i calcoli, ignorando il timebase dell'encoder:

```cpp
// NEW (OBS-style, dopo a0e255a)
ep.dts_usec = av_rescale(pkt->dts, 1'000'000, impl->cfg_fps);
ep.tb_num   = 1;
ep.tb_den   = impl->cfg_fps;
```

E in `VideoEncoder::open()`:
```cpp
ctx->time_base = {1, cfg.fps};    // prima di avcodec_open2
impl_->vsp.tb_num = 1;
impl_->vsp.tb_den = cfg.fps;
```

### 4. Perché il fix OBS-style non basta?

Il fix assume che l'encoder **passi attraverso** i PTS dei frame senza riscalarli. Ma molti encoder HW (NVENC, AMF) **cambiano `ctx->time_base`** dopo `avcodec_open2` e riscalano i PTS/DTS in uscita su quel nuovo timebase.

**Flusso sospetto con `hevc_amf` / `h264_nvenc`:**

```
1. Noi impostiamo:  ctx->time_base = {1, 60}     (per 60fps)
2. avcodec_open2:   encoder cambia a {1, 1000000} (microsecondi)
3. Noi mandiamo:    frame->pts = 0, 1, 2, 3...   (contatori sequenziali)
4. Encoder interpreta PTS=1 come 1μs (non 1/60s) → riscala tutto
5. Output pkt->dts: 0, 16666, 33333...           (in {1, 1000000})
6. Noi calcoliamo:  dts_usec = 16666 * 1M / 60 = 277.766.666 μs ← ERRORE enorme
```

Questo produrrebbe clip **cortissime** (time cap scatterebbe dopo 1-2 frame). Il fatto che invece le clip siano **2×** suggerisce che il timebase venga cambiato in modo specifico, probabilmente `{1, fps*2}` o simile, oppure che il pacer produca la metà dei frame attesi.

---

## Diagnostica in corso

### Logging temporaneo aggiunto (`monolith_enc_diag.txt`)

In `libs/encoding/encoding.cpp`:

1. **Dopo `avcodec_open2`** — logga `ctx->time_base.num/den` reali per video e audio
2. **In `drain_video()`** — logga i primi 20 pacchetti video: `dts`, `pts`, `dts_usec`, `cfg_fps`
3. **In `drain_audio()`** — logga i primi 10 pacchetti audio: `dts`, `pts`, `dts_usec`, `sample_rate`

### File di output

Il log viene scritto in `%TEMP%\monolith_enc_diag.txt` (es. `C:\Users\<nome>\AppData\Local\Temp\monolith_enc_diag.txt`).

### Cosa cercare nei log

| Linea | Significato |
|-------|-------------|
| `video time_base after avcodec_open2: N/D (cfg_fps=60, encoder=hevc_amf)` | Mostra il timebase **reale** dopo open2. Se `N/D` ≠ `1/60`, conferma la modifica dell'encoder. |
| `audio time_base after avcodec_open2: N/D (cfg_sample_rate=48000)` | L'audio dovrebbe mantenere `1/48000`. Se lo fa, conferma che solo il video ha il problema. |
| `video pkt #0: dts=0 pts=0 dts_usec=0 cfg_fps=60` | Primo pacchetto. `dts=0` è normale. |
| `video pkt #1: dts=X pts=Y dts_usec=Z cfg_fps=60` | Secondo pacchetto. **Valori attesi** se timebase `{1,60}`: `dts=1, dts_usec=16666`. **Valori sospetti**: `dts=16666` o `dts_usec=277766666`. |
| `audio pkt #0: dts=0 dts_usec=0 sr=48000` | Audio normale. |
| `audio pkt #1: dts=1024 dts_usec=21333 sr=48000` | Audio normale (1024 campioni in timebase `{1,48000}` = 21.3ms). |

### Interpretazione

| Scenario | Log | Conclusione |
|----------|-----|-------------|
| Timebase video = `{1, fps}` E `dts` = 0,1,2,... | Fix OBS funziona | Bug altrove (pacer? buffer?) |
| Timebase video ≠ `{1, fps}` E `dts` sequenziali piccoli | Encoder non riscala PTS | Basta usare `ctx->time_base` per stream timebase nel file |
| Timebase video ≠ `{1, fps}` E `dts` grandi (es. 16666) | Encoder riscala PTS | Fix complesso: frame->pts va riscalato al timebase reale |
| `dts_usec` identico tra pacchetti (es. sempre 0) | Encoder non produce DTS validi | Time cap non funziona, clip definite solo da memory cap |

---

## Prossimi passi

1. Raccogliere il log diagnostico dopo aver generato una clip
2. Analizzare i valori reali di `ctx->time_base` e `pkt->dts`
3. Applicare il fix definitivo:
   - Salvare `ctx->time_base` dopo `avcodec_open2` come `impl_->pkt_tb`
   - Usare `impl_->pkt_tb` per `dts_usec`, `tb_num/tb_den`, e `vsp.tb_num/tb_den`
   - Riscalare `frame->pts` al timebase reale dell'encoder prima di `avcodec_send_frame`
4. Rimuovere il logging diagnostico

---

## File coinvolti

- `libs/encoding/encoding.cpp` — Video/Audio encoder con fix OBS-style e logging diagnostico
- `libs/encoding/encoding.h` — Strutture dati (EncodedPacket, VideoStreamParams, AudioStreamParams)
- `libs/replay-buffer/replay_buffer.cpp` — Buffer ad anello, `find_clip_start()`, `write_clip()`
- `app/recorder/src/main.cpp` — Pacer thread, WGC callback, avvio encoder
