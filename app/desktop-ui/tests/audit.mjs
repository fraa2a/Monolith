import assert from "node:assert/strict";
import { readFile, mkdir } from "node:fs/promises";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { chromium } from "playwright-core";

const base = "http://127.0.0.1:4174";
const seed = JSON.parse(await readFile("../../config/default-config.json", "utf8"));
const server = spawn(process.execPath, ["node_modules/vite/bin/vite.js", "preview", "--host", "127.0.0.1", "--port", "4174", "--strictPort"], { stdio: "pipe" });
let serverError = "";
server.stderr.on("data", (chunk) => { serverError += chunk; });
let browser;
try {
  let ready = false;
  for (let attempt = 0; attempt < 100; attempt++) {
    if (server.exitCode !== null) throw new Error(`Preview exited: ${serverError}`);
    try { ready = (await fetch(base)).ok; } catch {}
    if (ready) break;
    await delay(100);
  }
  assert(ready, "Preview did not start");
  browser = await chromium.launch({ executablePath: process.env.UI_BROWSER_PATH || undefined, headless: true, args: ["--no-sandbox", "--disable-gpu"] });
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  if (process.env.UI_SCREENSHOT_DIR) await mkdir(process.env.UI_SCREENSHOT_DIR, { recursive: true });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.route("**/__fixtures__/**", async (route) => {
    if (route.request().url().includes("video")) {
      return route.fulfill({ status: 204 });
    }
    const id = Number(route.request().url().match(/(\d+)/g)?.at(-1) ?? 0);
    const hue = (id * 37) % 360;
    return route.fulfill({ contentType: "image/svg+xml", body: `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360"><defs><linearGradient id="bg" x2="1" y2="1"><stop stop-color="hsl(${hue},40%,24%)"/><stop offset="1" stop-color="#101820"/></linearGradient></defs><rect width="640" height="360" fill="url(#bg)"/><path d="M0 300 160 130 270 240 410 80 640 280V360H0Z" fill="#141c23"/><text x="24" y="42" fill="#eee" font-family="sans-serif" font-size="20">UI test fixture ${id}</text></svg>` });
  });
  await page.addInitScript((config) => {
    const clips = Array.from({ length: 15 }, (_, i) => ({ id: i + 1, source: "replay", catalog_uid: "catalog-replay", clip_uid: `clip${i + 1}`, media_revision: 0, video_file: `video${i + 1}.mkv`, title: `Match ${String(i + 1).padStart(2, "0")}`, thumbnail_file: `thumb${i + 1}.svg`, video_path: `video${i + 1}.mkv`, thumbnail_path: `thumb${i + 1}.svg`, created_at_utc: "2026-10-08T10:00:00Z", duration_seconds: 30, game_process_name: "arena.exe", game_display_name: "Arena", favorite: false, hashtags: ["gameplay"], size_bytes: 12000000 }));
    const collections = [{ id: 1, name: "Best moments", color: "#c4ff42", clip_count: 3, created_at_utc: "2026-10-08T10:00:00Z" }];
    let engine = { connected: true, recording: false, replay_enabled: true, capture_running: true, replay_running: true, recording_error: "", version: "1.6.2" };
    const bookmarks = [];
    const calls = [];
    window.__uiTest = { calls, config, engine, clips, collections, failCommand: false, unknown: [] };
    window.__uiTest.maximized=false; window.__uiTest.mediaLoads=0; window.__uiTest.saveDelay=0; window.__uiTest.activeSaves=0; window.__uiTest.maxSaves=0;
    HTMLMediaElement.prototype.load=function(){if(this.getAttribute('src'))window.__uiTest.mediaLoads++;};
    let callback = 0; const handlers = new Map(); const listeners = new Map();
    window.__uiTest.emit = (event = "clips") => { for (const [id, listener] of listeners) if (listener.event === event) handlers.get(listener.handler)?.({event, id, payload:null}); };
    window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener(event, id) { listeners.delete(id); } };
    window.__TAURI_INTERNALS__ = {
      metadata: { currentWindow: { label: "main" }, currentWebview: { label: "main" } },
      transformCallback: (fn) => { handlers.set(++callback, fn); return callback; },
      unregisterCallback() {},
      convertFileSrc: (path) => `/__fixtures__/${path}`,
      invoke: async (command, args = {}) => {
        calls.push({ command, args });
        switch (command) {
          case "list_clips": return structuredClone(clips.filter((clip) => (!args.filter.favorite || clip.favorite) && (!args.filter.search || clip.title.toLowerCase().includes(args.filter.search.toLowerCase())) && (!args.filter.game || args.filter.game === clip.game_display_name) && (!args.filter.hashtag || clip.hashtags.includes(args.filter.hashtag))));
          case "distinct_games": return ["Arena"];
          case "distinct_hashtags": return ["gameplay"];
          case "engine_status": return structuredClone(engine);
          case "get_settings": return structuredClone(config);
          case "save_settings": {window.__uiTest.activeSaves++; window.__uiTest.maxSaves=Math.max(window.__uiTest.maxSaves,window.__uiTest.activeSaves); await new Promise(r=>setTimeout(r,window.__uiTest.saveDelay)); window.__uiTest.activeSaves--; if(window.__uiTest.failSave)throw new Error('Settings unavailable'); Object.assign(config,structuredClone(args.config)); return;}
          case 'plugin:window|close': window.__uiTest.emit('tauri://close-requested'); return;
          case 'plugin:window|destroy': window.__uiTest.windowClosedConfig = structuredClone(config); return;
          case 'plugin:window|is_maximized': return window.__uiTest.maximized;
          case 'plugin:window|maximize': window.__uiTest.maximized=true; return;
          case 'plugin:window|unmaximize': window.__uiTest.maximized=false; return;
          case 'clip_snapshot': return structuredClone(clips.find(c=>c.source===args.source&&c.id===args.id));
          case 'thumb_capture': {const c=clips.find(c=>c.id===args.id); c.thumbnail_file='new.png'; c.thumbnail_path='new.png'; return 'new.png';}
          case 'clip_regen_thumb': {const c=clips.find(c=>c.id===args.id); c.thumbnail_file='engine.png'; c.thumbnail_path='engine.png'; return;}
          case 'clip_set_duration': return;
          case 'collection_memberships': return [1];
          case 'reveal_in_explorer': return;
          case "runtime_status": return { monitors: [{ device: "DISPLAY1", width: 1920, height: 1080, primary: true }], available_encoders: ["h264_nvenc"], input_devices: [], audio_sessions: [] };
          case "plugin:app|version": return "1.5.0";
          case "plugin:event|listen": listeners.set(callback, {event:args.event,handler:args.handler}); return callback;
          case "game_catalog_map": return {};
          case "game_icon":
          case "exe_icon": return null;
          case "game_artwork": return { icon: null, cover: null };
          case "clip_list_bookmarks": return structuredClone(bookmarks);
          case "clip_add_bookmark":
          case "clip_update_bookmark":
          case "clip_delete_bookmark":
          case "clip_trim": {
            const clip = clips.find((clip) => clip.id === args.id && clip.source === args.source);
            if (args.mediaRevision !== clip.media_revision) throw new Error("Stale media revision");
            if (command === "clip_add_bookmark") bookmarks.push({seq:1,time_seconds:args.timeSeconds,label:args.label,color:args.color});
            if (command === "clip_update_bookmark") Object.assign(bookmarks.find((bm)=>bm.seq===args.seq), {label:args.label,color:args.color});
            if (command === "clip_delete_bookmark") bookmarks.splice(bookmarks.findIndex((bm)=>bm.seq===args.seq),1);
            if (command === "clip_trim") { clip.duration_seconds=args.end-args.start; clip.media_revision++; }
            return;
          }
          case "clip_set_favorite": clips.find((clip) => clip.id === args.id).favorite = args.favorite; return;
          case "list_collections": return structuredClone(collections);
          case "collection_clips": return structuredClone(clips.slice(0, 3));
          case "create_collection": { const id = Math.max(...collections.map((c) => c.id), 0) + 1; collections.push({ id, name: args.name, color: args.color, clip_count: 0 }); return id; }
          case "rename_collection": collections.find((c) => c.id === args.id).name = args.name; return;
          case "delete_collection": collections.splice(collections.findIndex((c) => c.id === args.id), 1); return;
          case "recorder_command":
            if (window.__uiTest.failCommand) throw new Error("Recorder unavailable");
            if (args.method === "recording_start") engine.recording = true;
            if (args.method === "recording_stop") engine.recording = false;
            return;
          default:
            if (command.startsWith("plugin:window|") || command === "plugin:event|unlisten") return;
            window.__uiTest.unknown.push(command);
            throw new Error(`Unexpected mock command: ${command}`);
        }
      },
    };
  }, seed);
  await page.goto(base);
  async function until(fn, label) {
    for (let attempt = 0; attempt < 100; attempt++) {
      if (await fn()) return;
      await delay(50);
    }
    throw new Error(`Timed out: ${label}`);
  }


  await until(async () => await page.locator(".card").count() === 15, "library");
  const failures=[];
  async function check(name,run) {
    if (process.env.UI_AUDIT_FILTER && !name.includes(process.env.UI_AUDIT_FILTER)) return;
    await page.reload();
    await until(async()=>await page.locator('.card').count()===15,'library');
    try {await run(); console.log(`PASS ${name}`);} catch(error) {failures.push(`${name}: ${error.message}`); console.log(`FAIL ${name}: ${error.message}`);}
  }
  async function settings(){await page.getByRole('button',{name:'Settings',exact:true}).click(); await until(async()=>await page.locator('[role=switch]').count()===3,'settings');}
  const replay=()=>page.locator('.set-field').filter({hasText:'Replay buffer'}).getByRole('switch');
  for(const exit of ['Close settings','Escape','Library','Collections']) await check(`F01 flush ${exit}`,async()=>{
    await settings(); await replay().click();
    if(exit==='Escape')await page.keyboard.press('Escape'); else await page.getByRole('button',{name:exit,exact:true}).click();
    await delay(750); assert.equal(await page.evaluate(()=>window.__uiTest.config.replay_buffer.enabled),false);
  });
  await check('F01 close window flushes pending settings',async()=>{
    await settings();await page.evaluate(()=>window.__uiTest.saveDelay=300);await replay().click();
    await page.getByRole('button',{name:'Close',exact:true}).click();
    await until(async()=>await page.evaluate(()=>!!window.__uiTest.windowClosedConfig),'window close');
    assert.equal(await page.evaluate(()=>window.__uiTest.windowClosedConfig.replay_buffer.enabled),false);
  });
  await check('F01 native close waits and retains window on save error',async()=>{
    await settings();await page.evaluate(()=>{window.__uiTest.saveDelay=300;window.__uiTest.failSave=true;});await replay().click();
    await page.evaluate(()=>window.__uiTest.emit('tauri://close-requested'));
    await until(async()=>(await page.locator('[role=alert]').allTextContents()).some(s=>s.includes('Settings unavailable')),'close save failure');
    assert.equal(await page.evaluate(()=>!!window.__uiTest.windowClosedConfig),false);
    await page.evaluate(()=>{window.__uiTest.failSave=false;window.__uiTest.emit('tauri://close-requested');});
    await until(async()=>await page.evaluate(()=>!!window.__uiTest.windowClosedConfig),'native close retry');
    assert.equal(await page.evaluate(()=>window.__uiTest.windowClosedConfig.replay_buffer.enabled),false);
  });
  await check('F02 shared serialized writes',async()=>{
    await settings(); await page.evaluate(()=>window.__uiTest.saveDelay=900);
    await page.getByTitle('Capture source and mode').click(); await page.getByRole('button',{name:'Game Recording',exact:true}).click();
    await replay().click(); await delay(600); await page.getByRole('button',{name:'Close settings',exact:true}).click();
    await delay(2300);
    assert.equal(await page.evaluate(()=>window.__uiTest.config.capture_mode.mode),'game_only');
    assert.equal(await page.evaluate(()=>window.__uiTest.config.replay_buffer.enabled),false);
    assert.equal(await page.evaluate(()=>window.__uiTest.maxSaves),1);
  });
  await check('F02 failure visible after exit',async()=>{
    await settings(); await page.evaluate(()=>window.__uiTest.failSave=true); await replay().click(); await page.getByRole('button',{name:'Close settings',exact:true}).click();
    await until(async()=>(await page.locator('[role=alert]').allTextContents()).some(s=>s.includes('Settings unavailable')),'save failure');
  });
  await check('F03 detail identity insert and removal',async()=>{
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await page.evaluate(()=>{let t=window.__uiTest;t.clips.unshift({...t.clips[0],id:99,clip_uid:"clip99",title:'New clip'});t.emit();}); await delay(250);
    assert.equal(await page.locator('.detail-name').textContent(),'Match 01');
    await page.evaluate(()=>{let t=window.__uiTest;t.clips.splice(t.clips.findIndex(c=>c.id===1),1);t.emit();});
    await until(async()=>await page.locator('.detail').count()===0,'removed detail');
  });
  await check('F03 reused local ID never retargets detail',async()=>{
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await page.evaluate(()=>{window.__uiTest.clips[0]={...window.__uiTest.clips[0],clip_uid:'replacement',title:'Replacement'};window.__uiTest.emit();});
    await until(async()=>await page.locator('.detail').count()===0,'replaced clip identity');
  });
  await check('F03 legacy empty UIDs preserve individual clips',async()=>{
    await page.evaluate(()=>{window.__uiTest.clips.forEach(c=>{c.clip_uid='';c.catalog_uid='';});window.__uiTest.emit();});
    await page.getByRole('button',{name:'Open Match 02',exact:true}).click();assert.equal(await page.locator('.detail-name').textContent(),'Match 02');
    await page.evaluate(()=>{let t=window.__uiTest;t.clips.unshift({...t.clips[0],id:99,video_path:'other.mkv',title:'New clip'});t.emit();});await delay(250);
    assert.equal(await page.locator('.detail-name').textContent(),'Match 02');
  });
  await check('F04 clipping uses running pipeline and recording errors surface',async()=>{
    assert((await page.getByTitle('Capture source and mode').textContent()).includes('Clipping'));
    await page.evaluate(()=>{window.__uiTest.engine.replay_running=false;});
    await until(async()=>(await page.getByTitle('Capture source and mode').textContent()).includes('Idle'),'replay idle');
    await page.evaluate(()=>{window.__uiTest.engine.recording_error='Writer failed';});
    await until(async()=>(await page.locator('[role=alert]').allTextContents()).some(s=>s.includes('Writer failed')),'recording error');
  });
  await check('F05 arrows belong to focused text and sliders',async()=>{
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click(); await page.locator('.tag-input').fill('hello');
    await page.keyboard.press('ArrowLeft'); await page.keyboard.press('ArrowRight');
    assert.equal(await page.locator('.detail-name').textContent(),'Match 01');
    await page.locator('.detail input[type=range]').first().focus(); await page.keyboard.press('ArrowRight');
    assert.equal(await page.locator('.detail-name').textContent(),'Match 01');
  });
  await check('F07 unavailable track API warns and offers file',async()=>{
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await until(async()=>(await page.locator('[role=status]').allTextContents()).some(s=>s.includes('additional audio tracks')),'track capability notice');
    await page.getByRole('button',{name:'Show video file',exact:true}).click();
    assert.equal(await page.evaluate(()=>window.__uiTest.calls.filter(c=>c.command==='reveal_in_explorer').length),1);
  });
  await check('F08 collection event refresh',async()=>{
    await page.getByRole('button',{name:'Collections',exact:true}).click(); await page.getByRole('button',{name:'Best moments 3 clips'}).click();
    await until(async()=>await page.locator('.card').count()===3,'collection');
    await page.evaluate(()=>{window.__uiTest.clips[0].title='Changed';window.__uiTest.emit();});
    await until(async()=>await page.getByRole('button',{name:'Open Changed',exact:true}).count()===1,'collection refresh');
  });
  await check('F09 no reads for valid catalog metadata',async()=>{await delay(100);assert.equal(await page.evaluate(()=>window.__uiTest.mediaLoads),0);});
  await check('F09 bounded visible missing-media probes',async()=>{
    await page.route('**/__fixtures__/video*', () => {});
    await page.setViewportSize({width:800,height:600});
    await page.evaluate(()=>{window.__uiTest.clips.forEach(c=>{c.thumbnail_file=null;c.thumbnail_path=null;c.duration_seconds=null;});window.__uiTest.mediaLoads=0;window.__uiTest.emit();});
    await delay(250);assert(await page.evaluate(()=>window.__uiTest.mediaLoads)<=2,'at most two concurrent repair probes');
    await page.unroute("**/__fixtures__/video*");
    await page.setViewportSize({width:1920,height:1080});
  });
  await check('F10 named switch and select',async()=>{
    await settings(); assert.equal(await page.getByRole('switch',{name:'Replay buffer',exact:true}).count(),1);
    await page.getByRole('navigation',{name:'Settings pages'}).getByRole('button',{name:'Capture',exact:true}).click();
    assert.equal(await page.getByRole('combobox',{name:'Resolution',exact:true}).count(),1);
  });
  await check('F11 unfavorite exits filtered list',async()=>{
    await page.locator('.card').first().getByTitle('Add to favorites').click();await page.getByRole('button',{name:'Favorites',exact:true}).click();
    await until(async()=>await page.locator('.card').count()===1,'favorites');await page.locator('.card').getByTitle('Remove from favorites').click();
    await until(async()=>await page.locator('.card').count()===0,'unfavorite');
  });
  for(const maximized of [false,true])await check(`F12 restore window ${maximized}`,async()=>{
    await page.evaluate(v=>window.__uiTest.maximized=v,maximized);await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await page.locator('.detail').getByTitle('Fullscreen',{exact:true}).click();await delay(100);await page.keyboard.press('Escape');await delay(100);
    assert.equal(await page.evaluate(()=>window.__uiTest.maximized),maximized);
  });
  await check('Q03 membership reads remain compact',async()=>{
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await page.locator('.detail').getByTitle('Add to collection',{exact:true}).click();
    await until(async()=>await page.getByRole('button',{name:'In collection',exact:true}).count()===1,'membership picker');
    assert.equal(await page.evaluate(()=>window.__uiTest.calls.filter(c=>c.command==='collection_clips').length),0);
    assert.equal(await page.evaluate(()=>window.__uiTest.calls.filter(c=>c.command==='collection_memberships').length),1);
  });
  await check('S03 timeline mutations use the clip media revision',async()=>{
    await page.evaluate(()=>{window.__uiTest.clips[0].media_revision=7;window.__uiTest.clips[0].source="manual";window.__uiTest.emit();});
    await page.getByRole('button',{name:'Open Match 01',exact:true}).click();
    await page.evaluate(()=>{Object.defineProperty(HTMLMediaElement.prototype,'duration',{get:()=>30});document.querySelector('.detail video').dispatchEvent(new Event('loadedmetadata'));});
    await page.getByTitle('Add bookmark at current time',{exact:true}).click();
    await until(async()=>await page.getByTitle('Edit bookmark',{exact:true}).count()===1,'bookmark added');
    await page.getByTitle('Edit bookmark',{exact:true}).click();await page.locator('.bm-label-input').fill('Edited');await page.keyboard.press('Enter');
    await until(async()=>await page.locator('.bm-label').textContent()==='Edited','bookmark updated');
    await page.getByTitle('Delete bookmark',{exact:true}).click();await until(async()=>await page.getByTitle('Edit bookmark',{exact:true}).count()===0,'bookmark deleted');
    await page.getByTitle('Trim',{exact:true}).click();await page.locator('.trim-start').focus();await page.keyboard.press('ArrowRight');await page.getByRole('button',{name:'Apply',exact:true}).click();
    await until(async()=>await page.evaluate(()=>window.__uiTest.clips[0].media_revision)===8,'clip trimmed');
  });
  await check('Q02 transient media saturation retries thumbnail without regeneration',async()=>{
    await page.evaluate(() => {
      window.__retryFrame = window.requestAnimationFrame;
      window.requestAnimationFrame = (callback) => setTimeout(() => callback(performance.now()), 250);
    });
    let requests=0;
    await page.route('**/flaky.svg*',route=>{requests++;return requests===1?route.fulfill({status:503}):route.fulfill({contentType:'image/svg+xml',body:'<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="10" height="10" fill="white"/></svg>'});});
    await page.evaluate(()=>{window.__uiTest.clips[0].thumbnail_path='flaky.svg';window.__uiTest.emit();});
    await until(async()=>requests>=2&&await page.locator('.card').first().locator('.card-thumb').evaluate(img=>img.complete&&img.naturalWidth>0),'thumbnail retry');
    assert.equal(await page.evaluate(()=>window.__uiTest.calls.filter(c=>c.command==='clip_regen_thumb').length),0);
    await page.unroute('**/flaky.svg*');
    await page.evaluate(() => { window.requestAnimationFrame = window.__retryFrame; });
  });
  await check('F06 engine fallback restores missing thumbnail path',async()=>{
    await page.evaluate(()=>{const c=window.__uiTest.clips[0];c.thumbnail_file=null;c.thumbnail_path=null;c.video_file='unsupported-video.mkv';window.__uiTest.emit();});
    await until(async()=>await page.locator('.card').first().locator('.card-thumb').count()===1,'engine thumbnail');
    assert((await page.locator('.card').first().locator('.card-thumb').getAttribute('src')).includes('engine.png'));
  });
  await check('F06 generated thumbnail path displayed',async()=>{
    await page.evaluate(()=>{const c=window.__uiTest.clips[0];c.thumbnail_file=null;c.thumbnail_path=null;c.duration_seconds=null;window.__uiTest.emit();});
    await until(async()=>await page.locator('.card').first().locator('.thumb-placeholder').count()===1,'missing thumb');
    await page.evaluate(()=>{
      Object.defineProperty(HTMLMediaElement.prototype,'duration',{get:()=>30});Object.defineProperty(HTMLVideoElement.prototype,'videoWidth',{get:()=>640});Object.defineProperty(HTMLVideoElement.prototype,'videoHeight',{get:()=>360});CanvasRenderingContext2D.prototype.drawImage=()=>{};
      HTMLMediaElement.prototype.load=function(){if(!this.getAttribute('src'))return;setTimeout(()=>{this.dispatchEvent(new Event('loadedmetadata'));this.dispatchEvent(new Event('loadeddata'));},10);};
      window.__uiTest.clips[0].video_file='video-new.mkv';window.__uiTest.emit();
    });
    await until(async()=>await page.locator('.card').first().locator('.card-thumb').count()===1,'generated thumbnail');
  });
  assert.equal(errors.length,0,errors.join('\n'));assert.equal(failures.length,0,failures.join('\n'));
} finally {await browser?.close();server.kill();}
