# REPORT.md — Analisi duplicazioni, CPU/RAM e qualità video Monolith

> Vincolo rispettato: questo documento è solo un report. Non propone patch applicate e non modifica codice applicativo.
>
> Area analizzata: `app/recorder`, `libs/capture`, `libs/encoding`, `libs/replay-buffer`, `libs/recording`, `libs/audio`, `app/desktop-ui`, config e documentazione operativa.
>
> Obiettivo: identificare duplicazioni/overhead e modifiche efficienti, corrette e funzionanti per ridurre CPU, ridurre RAM e migliorare la qualità video a parità di livello qualità nelle Settings.

---

## 1. Executive summary

Monolith è già impostato correttamente su un principio importante: il replay buffer conserva pacchetti codificati, non frame raw. Dopo gli update recenti, il payload dei packet è ref-counted e lo snapshot del replay non duplica più l'intera finestra codificata.

Restano però duplicazioni e costi strutturali rilevanti:

1. Il frame video viene letto dalla GPU in una staging texture CPU-readable, poi copiato in un buffer BGRA del pacer, poi convertito da `sws_scale()` in frame encoder. Questa è la catena più costosa in CPU/RAM.
2. Il pacer mantiene almeno due buffer raw BGRA (`g_pacer_shared.bgra` e `local_bgra`). Non è un leak, ma è memoria duplicata per design.
3. Il muxing scrive i packet creando un nuovo `AVPacket` e copiando il payload ref-counted dentro FFmpeg. Questo non duplica più il replay snapshot, ma duplica temporaneamente ogni packet durante scrittura.
4. L'audio custom usa mixer continui per ogni track configurata. Questo garantisce allineamento e track stabili, ma può generare encode AAC di silenzio anche quando non c'è sorgente attiva.
5. La Settings UI ha iniziato a mostrare la shell prima, ma poi popola ancora Capture e Audio subito dopo il primo frame; quindi il lazy-load è parziale e alcune inizializzazioni vengono ripetute anche alla navigazione pagina.
6. L'enumerazione runtime/encoder/audio può essere ripetuta più volte tra recorder e Settings, con I/O su `runtime-status.json` e probing FFmpeg non cacheato in modo persistente.
7. La qualità video finale è limitata dalla pipeline BGRA CPU + conversione/scaling software + formato `YUV420P` fisso. A parità di valore qualità, si può migliorare visibilmente lavorando su conversione colore/scaling, color range, formati input encoder e GPU path, non semplicemente cambiando il numero qualità.

La priorità più efficace è:

1. Eliminare o ridurre la pipeline raw CPU BGRA.
2. Fare scaling/conversione prima del readback o direttamente su GPU.
3. Evitare lavoro video/audio quando non necessario.
4. Migliorare scaling/colorimetry/timebase/encoder options per qualità superiore a pari QP/CRF.

---

## 2. Mappa attuale della pipeline video e punti di duplicazione

### 2.1 Flusso attuale

Percorso principale:

```text
WGC frame
  -> ID3D11Texture2D GPU
  -> CopyResource verso staging texture CPU-readable
  -> Map staging texture
  -> callback con puntatore BGRA
  -> memcpy in g_pacer_shared.bgra
  -> swap con local_bgra nel pacer
  -> VideoEncoder::push_bgra(local_bgra)
  -> sws_scale BGRA -> encoder pix_fmt
  -> avcodec_send_frame / receive_packet
  -> copia AVPacket -> EncodedBytes
  -> replay buffer e manual recorder condividono packet ref-counted
```

### 2.2 Duplicazioni/copie video confermate

| Punto | File/funzione | Cosa succede | Impatto |
|---|---|---|---|
| GPU texture -> staging texture | `libs/capture/capture.cpp`, `CopyResource` | Copia frame intero GPU-side verso risorsa CPU-readable | Sync GPU/CPU, costo alto a FPS/resoluzione elevati |
| Staging mapped -> pacer shared | `app/recorder/src/main.cpp`, `pacer_push_frame()` | `memcpy` frame BGRA intero in `g_pacer_shared.bgra` | 8.3 MB/frame a 1080p, 33 MB/frame a 4K |
| Pacer shared + local | `main.cpp`, pacer thread | Due vector raw possibili: shared e local | RAM raw duplicata, ma evita copia consumer |
| BGRA -> encoder frame | `libs/encoding/encoding.cpp`, `sws_scale()` | Conversione/scaling CPU per ogni frame emesso | CPU proporzionale a FPS e risoluzione |
| Encoder packet -> EncodedBytes | `drain_video()` | Copia `AVPacket` FFmpeg in memoria Monolith ref-counted | Necessaria oggi per lifetime, ma ancora una copia |
| EncodedBytes -> mux AVPacket | `libs/encoding/mux_common.cpp`, `write_packet()` | `av_new_packet` + `memcpy` per scrivere | Duplica temporaneamente ogni packet in save/recording |

### 2.3 Duplicazioni eliminate o ridotte di recente

| Prima | Ora | Stato |
|---|---|---|
| `EncodedPacket` con `std::vector<uint8_t>` copiava payload a ogni copia | `EncodedBytesRef` condiviso | Buono |
| `save_clip()` duplicava lo snapshot payload-sized | Snapshot copia riferimenti | Buono |
| WGC poteva leggere più frame del target FPS | `CaptureOptions` + `FrameGate` prima del readback | Buono, ma migliorabile |

---

## 3. Punti in cui viene generato lavoro duplicato o ridondante

## 3.1 Video raw buffer duplicato tra capture e pacer

### Evidenza

- `DisplayCapture` espone BGRA mappato dalla staging texture.
- `pacer_push_frame()` copia tutto in `g_pacer_shared.bgra`.
- Il pacer mantiene `local_bgra` per riemettere l'ultimo frame.

### Perché esiste

Il puntatore mappato è valido solo durante la callback. Serve una copia se il pacer deve usare il frame dopo `Unmap()`.

### Perché è costoso

A 1080p60:

```text
~8.3 MB * 60 = ~498 MB/s di memcpy solo capture -> pacer
```

A 4K60:

```text
~33 MB * 60 = ~2 GB/s
```

### Miglioramento consigliato

**Short-term corretto:** mantenere la copia ma ridurre risoluzione readback tramite GPU downscale prima del Map.

**Medium-term:** introdurre un ring/triple-buffer di staging texture + fence/event e passare ownership di un buffer CPU già persistente, evitando una copia extra quando possibile.

**Long-term:** non copiare raw frame su CPU: passare D3D11 texture all'encoder hardware.

---

## 3.2 Readback alla risoluzione sorgente anche quando output è inferiore

### Evidenza

`capture.cpp` crea staging texture con `fi.width`/`fi.height`, cioè dimensioni WGC/source. Il downscale avviene poi in `VideoEncoder::push_bgra()` via `sws_scale()`.

### Impatto

Se monitor 4K e output 1080p:

```text
Readback attuale: 33 MB/frame
Readback desiderabile: 8.3 MB/frame
Riduzione potenziale: ~4x memoria + bus + cache
```

Se output 720p:

```text
33 MB -> 3.7 MB
Riduzione potenziale: ~9x
```

### Miglioramento consigliato

Aggiungere pass D3D11 di downscale/render-to-texture prima dello staging:

```text
WGC texture source
  -> shader/VideoProcessorBlt a output size
  -> staging output-sized
  -> Map
```

Questo riduce CPU e RAM bandwidth senza cambiare il valore qualità nelle Settings. Inoltre migliora qualità rispetto a scaling CPU bilinear se si usa un filtro GPU di qualità decente.

---

## 3.3 Conversione colore/scaling CPU sempre eseguita per frame

### Evidenza

`VideoEncoder::push_bgra()` crea sempre uno `SwsContext` BGRA -> `ctx->pix_fmt`, poi chiama `sws_scale()`.

### Impatto

È uno dei costi più grandi e scala quasi linearmente con FPS. Anche quando risoluzione input=output, `sws_scale()` fa conversione colore BGRA -> YUV420P.

### Miglioramento consigliato

1. Separare scaling e color conversion:
   - se dimensioni uguali, usare conversione più diretta o shader GPU BGRA->NV12.
   - se dimensioni diverse, fare scaling GPU.
2. Usare formati encoder più adatti:
   - hardware encoders spesso preferiscono NV12/P010.
   - produrre NV12 su GPU migliora CPU e può migliorare qualità colore.
3. Impostare `sws_setColorspaceDetails()` se si resta su CPU path, per range/matrix coerenti.

---

## 3.4 Packet payload copiato due volte nel ciclo encode -> mux

### Evidenza

- `drain_video()` copia da `AVPacket` in `EncodedBytes`.
- `mux_common::write_packet()` crea un nuovo `AVPacket` e copia `ep.data()` dentro `pkt->data`.

### Impatto

Non causa più spike replay-sized, ma ogni packet codificato viene ancora copiato:

```text
FFmpeg encoder packet -> Monolith EncodedBytes -> FFmpeg mux packet
```

### Miglioramento consigliato

Usare packet lifetime nativo FFmpeg con `AVPacket` ref-counted oppure creare `AVPacket` wrapper in `EncodedPacket`:

```text
EncodedPacket holds av_packet_ref()-managed AVPacket
mux_common writes using av_packet_ref/av_interleaved_write_frame
```

Benefici:

- meno copie packet;
- meno allocazioni `unique_ptr<uint8_t[]>`;
- mux più diretto;
- utile soprattutto con bitrate alto e molte tracce.

Rischio:

- Espone FFmpeg lifetime fuori dal modulo encoding. Serve RAII wrapper ben isolato.

Soluzione intermedia:

- mantenere `EncodedBytesRef` per API pubblica, ma aggiungere `mux::write_packet_ref()` che usa buffer esterno dove possibile. Va verificato con FFmpeg: `av_packet_from_data` prende ownership, quindi non è adatto direttamente a `shared_ptr` const; meglio AVPacket RAII.

---

## 3.5 Audio mixer continuo anche per una sola sorgente o sorgente assente

### Evidenza

`make_routes()` manda sempre le sorgenti attraverso mixer; i commenti indicano che ogni track configurata ha mixer continuo. `TrackMixer` emette sempre PCM, anche silenzio, per mantenere track presenti/allineate.

### Impatto positivo

- Track stabili.
- A/V sync robusto.
- Multiple source per track finalmente sommabili.

### Impatto negativo

- AAC encoder lavora anche quando la track è silenziosa.
- Custom mode con più track può aprire fino a 6 mixer + 6 encoder.
- Per track con una sola sorgente, si paga comunque mixer + resampling + FIFO + thread.

### Miglioramento consigliato

Implementare tre modalità per track:

1. **Direct single-source mode**: una sorgente attiva -> encoder diretto, niente mixer.
2. **Mixed mode**: due o più sorgenti -> mixer.
3. **Silent placeholder mode**: solo se serve preservare una track configurata senza sorgenti live.

Regola efficiente:

```text
Se recording/replay non ha ancora scritto header: si può decidere dinamicamente.
Se header già scritto: track presenti devono restare coerenti.
```

Per default mode desktop+mic, nessun mixer dovrebbe essere necessario.

---

## 3.6 Runtime status e probing ripetuti

### Evidenza

`media_start()`:

- enumera monitor;
- chiama `available_video_encoders(1920,1080)`.

Settings:

- legge `runtime-status.json`;
- all'apertura/navigazione può ricaricare runtime status;
- Capture/Audio vengono popolati anche subito dopo first frame e poi di nuovo in `InitializePageAsync()`.

### Impatto

- Probing encoder FFmpeg è costoso, specialmente hardware encoder.
- Enumerazioni audio/sessioni possono essere costose.
- Settings cold-start migliora solo parzialmente se dopo first frame carica comunque tutto.

### Miglioramento consigliato

1. Cache encoder availability con chiave:

```text
encoder_probe_key = gpu/driver/process arch + width class + codec list + app version
```

2. In Settings, non popolare Capture/Audio dopo first frame. Popolare solo pagina General/Output/Clip minima.
3. `InitializePageAsync()` deve essere l'unico punto di init pagina pesante.
4. Runtime status: usare timestamp e debounce; leggere file solo se `LastWriteTimeUtc` cambia.

---

## 4. Miglioramenti CPU ad alto impatto

## 4.1 D3D11 GPU downscale prima del readback

### Categoria

CPU ↓, RAM bandwidth ↓, qualità video ↑

### Descrizione

Quando output resolution è custom inferiore alla source, fare downscale su GPU prima di creare/mappare staging texture.

### Perché migliora qualità

Lo scaling CPU attuale usa `sws_scale()` con filtro configurabile ma spesso bilinear. Un D3D11 video processor o shader bicubic/Lanczos separabile può produrre downscale più pulito a pari qualità encoder:

- meno aliasing;
- testo/UI più leggibile;
- dettagli fini meno distrutti;
- input encoder già alla risoluzione finale.

### Perché riduce CPU/RAM

Riduce bytes copiati/mappati e conversione CPU. Anche se resta BGRA CPU, il frame raw è più piccolo.

### Implementazione consigliata

- In `libs/capture`, aggiungere opzioni output size.
- Creare render target texture output-sized.
- Usare D3D11 video processor o shader full-screen triangle.
- Staging texture dimensionata all'output, non source.
- Se output=source, bypass.

### Rischi

- gestione format/color space;
- device/context reuse;
- resize monitor;
- border/cursor behavior WGC invariato.

---

## 4.2 Encoder hardware con D3D11/NV12 surfaces

### Categoria

CPU ↓↓↓, RAM ↓↓, qualità video ↑

### Descrizione

Aggiungere path in cui WGC texture resta GPU-resident e viene convertita/downscalata a NV12/P010 su GPU, poi data a encoder hardware.

### Perché migliora qualità a pari setting

L'encoder hardware riceve un formato nativo più coerente, con meno passaggi:

```text
Attuale: BGRA WGC -> CPU BGRA -> sws YUV420P -> encoder
Target: BGRA WGC -> GPU NV12/P010 -> encoder
```

Meno conversioni = meno errori di rounding, meno chroma artifacts, possibilità P010/10-bit per HEVC se futuro.

### Implementazione consigliata

Stadi:

1. Aggiungere API parallela:

```cpp
VideoEncoder::open_d3d11(...)
VideoEncoder::push_d3d11_texture(...)
```

2. Tenere CPU path fallback.
3. Prima implementazione: Media Foundation H.264 D3D11 surfaces oppure FFmpeg AVHWFramesContext D3D11.
4. Pubblicare gli stessi `EncodedPacket` verso replay/recording.

### Rischi

- timestamp e B-frame;
- extradata SPS/PPS;
- mux compatibility;
- fallback robusto se encoder non supporta D3D11.

---

## 4.3 Separare capture FPS da encode FPS e monitor refresh

### Categoria

CPU ↓

### Stato attuale

È stato introdotto `max_readback_fps = video_fps`, ottimo primo passo.

### Miglioramento ulteriore

Per CFR, non sempre serve readback esattamente a encode FPS se il contenuto è statico o WGC fornisce duplicate. Si può usare dirty-region/present metadata se disponibile, o almeno drop static frames con hash leggero GPU/CPU limitato.

Possibili approcci:

1. **Adaptive readback**:
   - se frame content invariato per N frame, readback meno frequente e pacer duplica l'ultimo frame.
   - attenzione: nei giochi non bisogna introdurre stutter.

2. **Frame gate con deadline pacer condivisa**:
   - capture accetta frame solo se il pacer avrà bisogno di un nuovo frame entro il prossimo slot.
   - riduce readback appena prima/oltre deadline.

3. **Present timestamp aware**:
   - usare timestamp WGC anziché solo QPC locale, se affidabile.

---

## 4.4 Pacer wait fino al prossimo deadline reale

### Categoria

CPU ↓

### Stato attuale

Il pacer si sveglia a circa 2x FPS. Questo è più leggero di busy loop ma ancora più wakeups del necessario.

### Miglioramento consigliato

Calcolare deadline del prossimo frame e impostare waitable timer a quel deadline. Emettere al massimo un frame per wake.

Benefici:

- meno wakeups;
- meno jitter;
- meno CPU in idle/replay statico;
- meno bisogno di `timeBeginPeriod`.

---

## 4.5 Non avviare audio se si sta partendo solo per stato runtime

### Categoria

CPU ↓

### Evidenza

`media_start()` prima enumera monitor/encoder/status e poi, se video necessario, avvia audio e capture. Se replay disabled + recording idle ora ritorna prima dell'audio, bene.

### Miglioramento

Separare in modo netto:

- `publish_runtime_capabilities()`
- `start_video_pipeline()`
- `start_audio_pipeline()`
- `start_recording_session()`

Così Settings può ottenere capability senza avviare pipeline media e senza side-effect.

---

## 5. Miglioramenti RAM ad alto impatto

## 5.1 Eliminare doppio raw BGRA buffer con GPU path

### Categoria

RAM ↓↓↓

Il doppio buffer raw è necessario finché il pacer lavora su CPU BGRA. La vera eliminazione arriva con D3D11 texture path.

Target:

```text
latest_frame = ComPtr<ID3D11Texture2D> + timestamp
pacer/encoder queue = texture refs, non vector<uint8_t>
```

Con hardware encoder, la RAM privata scende perché non si allocano frame raw CPU persistenti.

---

## 5.2 Staging texture output-sized

### Categoria

RAM ↓↓

Se non si implementa subito GPU encoder, almeno la staging texture deve essere output-sized quando output < source.

Esempio 4K -> 1080p:

- staging attuale: ~33 MB;
- staging target: ~8 MB;
- pacer shared/local: da 66 MB potenziali a 16 MB.

---

## 5.3 Mux packet zero-copy/AVPacket RAII

### Categoria

RAM ↓, CPU ↓

Il ref-counted payload ha risolto il problema grande. La prossima ottimizzazione è rimuovere la copia `EncodedBytes -> AVPacket` durante mux.

Approccio consigliato:

```cpp
class EncodedPacket {
  AVPacket* pkt; // av_packet_ref lifetime
}
```

Oppure wrapper interno con API senza esporre FFmpeg ai moduli non-encoding.

Benefici:

- meno allocazioni per packet;
- meno memcpy;
- meno picchi durante save/recording ad alto bitrate.

---

## 5.4 Audio mixer FIFO allocation per track/sorgente

### Categoria

RAM ↓

`TrackMixer::add_source()` alloca FIFO da `out_rate` frame per source. A 48k stereo float non è enorme, ma con più sorgenti/track cresce.

Miglioramenti:

- allocazione FIFO iniziale più piccola con crescita limitata;
- direct mode per single-source;
- chiusura mixer/encoder silent dopo timeout se track non viene realmente usata e header non ancora scritto.

---

## 6. Miglioramenti qualità video a parità di valore qualità

Queste modifiche migliorano il risultato senza dire semplicemente "abbassa/alza quality 20".

## 6.1 Color range e colorspace espliciti

### Problema

Il codice imposta `ctx->pix_fmt = AV_PIX_FMT_YUV420P` e usa `sws_scale()` senza configurazione esplicita di matrix/range. Questo può causare mismatch RGB full range -> YUV limited/full non dichiarato correttamente.

### Effetto visivo

- neri leggermente sollevati o schiacciati;
- colori meno fedeli;
- banding/contrast shift;
- testo/UI meno nitido.

### Modifica consigliata

Impostare esplicitamente:

- `ctx->color_range`
- `ctx->colorspace`
- `ctx->color_primaries`
- `ctx->color_trc`
- `sws_setColorspaceDetails()` coerente.

Per SDR Windows desktop/gaming comune:

- BT.709 per 720p+;
- limited range per compatibilità video standard, oppure full range se dichiarato correttamente e player target lo supporta.

### Beneficio

Qualità percepita superiore a pari bitrate/QP perché il video non perde contrasto/colorimetria per conversione implicita.

---

## 6.2 Chroma siting e formato NV12 nativo per hardware encoder

### Problema

La path CPU genera YUV420P planar. Gli encoder hardware spesso lavorano nativamente meglio con NV12.

### Modifica consigliata

Per hardware encoder:

- usare NV12 come input se supportato;
- meglio ancora produrlo su GPU.

### Beneficio

Meno conversioni interne dentro encoder/driver, meno rischio di chroma artifacts, CPU inferiore.

---

## 6.3 Scaling di qualità superiore ma efficiente

### Problema

Il default bilinear è veloce ma produce blur/aliasing. Lanczos CPU migliora qualità ma costa CPU.

### Modifica consigliata

Implementare scaler GPU:

- bilinear GPU per Light;
- bicubic separabile GPU per Balanced/Quality;
- Lanczos/FSR-like sharpening opzionale per downscale importante.

### Beneficio

A pari encoder quality, il frame in ingresso è migliore. L'encoder spreca meno bit su aliasing e preserva dettagli reali.

---

## 6.4 Pre-processing leggero: dithering e sharpen controllato solo in downscale

### Categoria

Qualità ↑ senza aumentare bitrate

Quando si riduce risoluzione, un filtro di sharpen leggero post-downscale può migliorare leggibilità e percezione, ma va controllato per non generare ringing.

Consiglio:

- parametro interno non esposto inizialmente;
- applicare solo quando source/output ratio > 1.2;
- no sharpen se output=source.

---

## 6.5 Keyframe/GOP adattivo per clipping

### Problema

`gop_size = fps * 2`. È ragionevole, ma per replay/clip, keyframe ogni 2 secondi può rendere l'inizio clip meno preciso e può aumentare dipendenza da GOP lunghi.

### Miglioramento qualità/funzionalità

- Forzare IDR in momenti utili? Per replay non si sa in anticipo quando salverai.
- GOP 1s può migliorare seek/start clip ma aumenta bitrate a pari QP.
- Alternativa: mantenere 2s ma migliorare purge e clip start già fatto.

Consiglio:

- non cambiare default subito;
- esporre internamente `gop_seconds` e testare 1s vs 2s su qualità/size.

---

## 6.6 B-frame e preset encoder per qualità a pari QP

### Problema

Per software x264 viene impostato `tune=zerolatency`, che spesso disabilita B-frame e riduce efficienza/qualità a pari bitrate/CRF. Per recording/replay locale, la latenza sub-frame non è critica quanto nello streaming live.

### Modifica consigliata

Distinguere modalità:

- recording/replay: preset quality/latency balanced, B-frame permessi se mux/timestamp gestiti;
- low-latency/debug: zerolatency.

Per NVENC/AMF/QSV, valutare preset quality più efficiente a pari QP se CPU/GPU lo consentono.

### Beneficio

A parità di quality number, più efficienza di compressione e meno macroblocking.

### Rischio

B-frame richiedono timestamp/mux corretti. Il codice ordina per DTS nel save, ma va validato su recording live.

---

## 7. Problemi di correttezza o rischio da monitorare

## 7.1 `CaptureOptions` aggregate initialization fragile

Nel call site viene usato:

```cpp
capture::CaptureOptions{ show_border, fps, false }
```

Funziona finché l'ordine dei campi resta identico. Per robustezza futura, usare named construction esplicita.

## 7.2 `FrameGate` e WGC `MinUpdateInterval` possono ridurre fluidità se timer drift

La gate usa QPC locale. Va verificato su 144/165 Hz con output 60/30 che non introduca pacing irregolare. La telemetry `perf-video` aiuta, ma serve anche verifica visiva.

## 7.3 `media_start()` fa probing encoder prima di sapere se video serve?

Attualmente enumera monitor e probing encoder prima del check replay/recording idle. Quindi anche quando video pipeline non parte, viene comunque fatto `available_video_encoders(1920,1080)`. Questo è CPU cost a startup o recording start path.

Miglioramento: spostare il check `needs_video_pipeline()` prima del probing encoder pesante, oppure separare capability publishing.

## 7.4 Settings lazy-load incompleto

Dopo first frame, `MainWindow` chiama ancora:

- `PopulateCaptureCombos()`;
- `PopulateAudioControls()`;
- `RefreshAudioSourcesList()`.

Poi `InitializePageAsync()` può rifarlo alla navigazione. Questa è duplicazione di lavoro UI/runtime.

## 7.5 Audio custom sempre mixer

La documentazione commenta single-source direct, ma il codice crea mixer per ogni route. Verificare se questa scelta è intenzionale post-fix; è corretta per continuous tracks ma costosa.

---

## 8. Roadmap raccomandata per risultati massimi

## Fase A — Quick wins sicuri

1. Spostare `needs_video_pipeline()` prima di encoder probing in `media_start()`.
2. Completare lazy-load Settings: dopo first frame solo config base, niente Capture/Audio.
3. Cache `runtime-status.json` in Settings usando `LastWriteTimeUtc`.
4. Direct audio route per single-source tracks nel default mode.
5. Telemetry separata per `sws_scale` e `avcodec_send_frame` dentro `VideoEncoder`, non solo `push_bgra` aggregato.

Risultato atteso:

- CPU startup/settings inferiore;
- meno lavoro audio inutile;
- diagnosi più precisa.

## Fase B — Riduzione grossa CPU/RAM senza GPU encoder completo

1. D3D11 downscale before readback.
2. Staging texture output-sized.
3. GPU scaler migliore del bilinear CPU.
4. Colorimetry/range espliciti in CPU encoder path.
5. NV12 CPU/GPU intermediate per hardware encoder, se fattibile.

Risultato atteso:

- 4K->1080p molto più leggero;
- qualità superiore a pari quality;
- meno bandwidth e meno `sws_scale`.

## Fase C — Architettura competitiva

1. D3D11 texture encoder path.
2. AVPacket RAII/ref-counted native packet path.
3. Separazione `raw CPU output` vs `GPU encoder output`.
4. Capability-gated UI per GPU path.
5. Profiling persistente: dropped/readback/sws/encode/mux/audio.

Risultato atteso:

- CPU molto più bassa a 1080p60/1440p60/4K60;
- RAM privata più bassa;
- qualità migliore grazie a meno conversioni e formati nativi.

---

## 9. Priorità per impatto

| Priorità | Modifica | CPU | RAM | Qualità | Rischio |
|---:|---|---:|---:|---:|---:|
| 1 | GPU downscale before readback | Alto | Alto | Alto | Medio |
| 2 | D3D11 hardware encoder path | Molto alto | Molto alto | Alto | Alto |
| 3 | Colorimetry/range espliciti | Basso | Neutro | Alto | Medio |
| 4 | Settings lazy-load reale | Medio startup | Basso | Neutro | Basso |
| 5 | Direct audio route per single-source | Medio audio | Basso | Neutro | Medio |
| 6 | AVPacket RAII zero-copy mux | Medio | Medio | Neutro | Medio |
| 7 | Deadline pacer one-shot | Basso/Medio | Neutro | Neutro | Basso |
| 8 | Encoder presets con B-frame per recording | Basso | Neutro | Medio/Alto | Medio |

---

## 10. Conclusione

I duplicati più importanti non sono bug banali, ma conseguenze della pipeline CPU-first:

```text
GPU texture -> CPU staging -> BGRA vector -> second BGRA vector -> sws_scale frame -> encoded packet -> mux packet copy
```

Le ultime modifiche hanno già eliminato la duplicazione più grave nel replay snapshot. Il prossimo salto reale richiede ridurre/eliminare il raw BGRA CPU path.

La modifica con miglior rapporto impatto/rischio è **GPU downscale before readback**: riduce CPU/RAM e migliora qualità a pari setting senza riscrivere subito tutto l'encoder. La modifica definitiva è **D3D11 hardware encoder path**, mantenendo CPU BGRA come fallback.

Per la qualità video, le aree più promettenti sono:

- color range/matrix espliciti;
- scaling GPU di qualità;
- input NV12/P010 nativo per hardware encoder;
- preset encoder non forzatamente low-latency quando si registra su file.

Queste modifiche migliorano il video finale a parità di valore qualità nelle Settings perché migliorano il frame che arriva all'encoder e riducono conversioni/interpolazioni distruttive, non perché cambiano semplicemente il numero di qualità.
