// ─────────────────────────────────────────────────────────────────────────
//  The vision page, served from this board at GET /vision.
//
//  Why it lives here rather than on the dashboard: the stream is HTTP on the
//  LAN and the deployed dashboard is HTTPS, and browsers refuse that mix. A
//  page served from this board is same-scheme with the stream, and an HTTP
//  page may still call an HTTPS API — only the reverse is blocked.
//
//  The browser does the inference. MediaPipe's face landmarker runs in WASM,
//  reads frames from the MJPEG stream on port 81, decides focused/distracted/
//  absent, and posts batches back to POST /focus on this board, which attaches
//  the API token and forwards them. The token never reaches the page.
// ─────────────────────────────────────────────────────────────────────────

#pragma once

static const char VISION_PAGE[] PROGMEM = R"HTMLPAGE(
<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cadence vision</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; padding:16px; background:#0d0d0d; color:#ededed;
         font-family:system-ui,-apple-system,"Segoe UI",sans-serif; }
  h1 { font-size:15px; margin:0 0 12px; font-weight:600; }
  .wrap { display:flex; flex-wrap:wrap; gap:16px; align-items:flex-start; }
  canvas { max-width:100%; border-radius:8px; background:#000; }
  .panel { min-width:220px; }
  .state { font-size:34px; font-weight:600; margin:4px 0 2px; }
  .focused { color:#0ca30c; } .distracted { color:#fab219; } .absent { color:#898781; }
  .row { display:flex; justify-content:space-between; gap:12px;
         border-bottom:1px solid rgba(255,255,255,.1); padding:5px 0; font-size:13px; }
  .k { color:#898781; } .v { font-variant-numeric:tabular-nums; }
  .sess-on { color:#0ca30c; font-weight:600; } .sess-off { color:#898781; }
  /* Idle is the resting state, so it must not look like a fault. Dimming the
     readouts says "not recording" without implying "broken". */
  .idle .state, .idle #yaw, .idle #pitch, .idle #gaze { opacity:.55; }
  .note { color:#898781; font-size:12px; margin-top:10px; line-height:1.5; }
  button { background:#3987e5; color:#fff; border:0; border-radius:6px;
           padding:7px 12px; font-size:13px; cursor:pointer; margin-top:10px; }
  img { display:none; }
</style>

<h1>Cadence vision</h1>
<div class="wrap">
  <div>
    <canvas id="cv" width="640" height="480"></canvas>
    <img id="src" crossorigin="anonymous">
  </div>
  <div class="panel">
    <div class="k" style="font-size:12px">Current state</div>
    <div class="state absent" id="state">starting</div>
    <div class="row"><span class="k">session</span><span class="v sess-off" id="sess">checking</span></div>
    <div class="row"><span class="k">yaw</span><span class="v" id="yaw">-</span></div>
    <div class="row"><span class="k">pitch</span><span class="v" id="pitch">-</span></div>
    <div class="row"><span class="k">gaze</span><span class="v" id="gaze">-</span></div>
    <div class="row"><span class="k">eyes</span><span class="v" id="eyes">-</span></div>
    <div class="row"><span class="k">inference</span><span class="v" id="ms">-</span></div>
    <div class="row"><span class="k">tick rate</span><span class="v" id="rate">-</span></div>
    <div class="row"><span class="k">samples sent</span><span class="v" id="sent">0</span></div>
    <div class="row"><span class="k">last post</span><span class="v" id="post">-</span></div>
    <button id="cal">Calibrate centre</button>
    <p class="note">
      Look straight at your screen and press calibrate. Everything is measured
      relative to that, so the camera does not need to be centred.
    </p>
    <p class="note">
      This page previews and calibrates. It does not record — the headless host
      in <code>vision/</code> does, so the hub's focus-timer button works with
      no tab open. Add <code>?free=1</code> to record from here instead, with
      the host stopped.
    </p>
    <p class="note" id="err"></p>
  </div>
</div>

<script type="module">
// Pinned to a version that exists. 1.0.1 is current; an earlier draft of this
// page pinned 0.10.14, which is not a published version at all — the import
// 404s and the page dies before any of the code below runs, with nothing
// useful on screen to say why. Check the CDN, do not assume the version.
import { FaceLandmarker, FilesetResolver }
  from "https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@1.0.1/vision_bundle.mjs";

const $ = id => document.getElementById(id);
const cv = $("cv"), ctx = cv.getContext("2d", { willReadFrequently: true });
const img = $("src");

// Tunables, overridable from the query string so calibration does not need an
// edit-and-reflash cycle: /vision?yaw=0.22&hz=4&window=2000
const P = new URLSearchParams(location.search);
const YAW_LIMIT   = parseFloat(P.get("yaw")   ?? "0.20");  // fraction of eye span
const PITCH_LIMIT = parseFloat(P.get("pitch") ?? "0.28");
const GAZE_LIMIT  = parseFloat(P.get("gaze")  ?? "0.35");  // blendshape score
const HZ          = parseFloat(P.get("hz")    ?? "4");     // inference rate
const WINDOW_MS   = parseInt  (P.get("window")?? "2000");  // one sample per window
const POST_MS     = parseInt  (P.get("post")  ?? "10000"); // batch upload interval
const SESS_MS     = parseInt  (P.get("sess")  ?? "2000");  // session poll interval

// Recording normally follows the hub's focus-timer button. `?free=1` cuts that
// link and records continuously, which is how this page was used through all
// of Phase 4 and is still the right mode for calibrating on a bare bench with
// no hub running. It is deliberately not the default: samples that belong to
// no session are what made a closed tab look like four hours of focus.
const FREE_RUN    = P.get("free") === "1";

// The stream is on port 81; this page is served from port 80 on the same host.
img.src = `http://${location.hostname}:81/stream`;

let landmarker = null, centre = null, sent = 0, lastGaze = 0;
const votes = [];
const pending = [];
let tickCount = 0, rateAt = performance.now();

// ── session gating ───────────────────────────────────────────────────────
//
// The hub's focus-timer button is what starts and stops recording. This page
// cannot see that button, and the hub cannot address this tab, so both of
// them meet at GET /session on the camera board: the hub writes the state,
// this page reads it.
//
// The page stays loaded either way. The model is ~7 MB and the stream takes a
// moment to come up, so tearing either down between sessions would put that
// cost on the start of every session — where it would look exactly like the
// button not working. Instead it idles: preview live, calibration available,
// nothing recorded.

let sampling = false;

function setSampling(on, label) {
  const el = $("sess");
  el.textContent = label;
  el.className = "v " + (on ? "sess-on" : "sess-off");
  document.body.classList.toggle("idle", !on);

  if (on === sampling) return;
  sampling = on;

  if (on) {
    // Frames seen before the button was pressed belong to no session.
    votes.length = 0;
  } else {
    // Close the open window and get everything up now rather than holding a
    // partial batch behind the next timer. Whatever is still in `pending`
    // when the tab closes never existed as far as the database is concerned.
    flush();
    post();
  }
}

async function pollSession() {
  if (FREE_RUN) { setSampling(true, "free-run"); return; }
  try {
    const r = await fetch("/session", { cache: "no-store" });
    const j = await r.json();
    // Preview only. vision/cadence_vision.py is the recorder now — it runs
    // headless so the focus-timer button works without a tab being open.
    //
    // This page must NOT also sample. Both would classify the same seconds and
    // post them under timestamps a few milliseconds apart, which the
    // (device_id, ts) unique index cannot collapse, so every attention figure
    // would quietly double whenever this tab happened to be open. Use ?free=1
    // to record from here deliberately, with the host stopped.
    setSampling(false, j.state + " - preview only");
  } catch (e) {
    // One failed poll is not a stopped session — the board expires the state
    // by itself if the hub has really gone away, so keep doing whatever we
    // were doing and just say the link is down.
    $("sess").textContent = "board unreachable";
  }
}

try {
  const fileset = await FilesetResolver.forVisionTasks(
    "https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@1.0.1/wasm");
  landmarker = await FaceLandmarker.createFromOptions(fileset, {
    baseOptions: {
      modelAssetPath:
        "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task",
      delegate: "GPU",
    },
    outputFaceBlendshapes: true,
    runningMode: "IMAGE",
    numFaces: 1,
  });
} catch (e) {
  $("err").textContent = "Model failed to load: " + e.message +
    " — this page needs internet access for the MediaPipe model.";
}

// Head pose from landmark geometry rather than the transformation matrix.
// The matrix's axis convention varies between versions; the ratio of the
// nose's offset to the eye span does not, and it is scale-invariant, so it
// does not care how far away you sit.
function pose(lm) {
  const L = lm[33], R = lm[263], nose = lm[1], brow = lm[168], chin = lm[152];
  const span = Math.hypot(R.x - L.x, R.y - L.y) || 1e-6;
  const midX = (L.x + R.x) / 2, midY = (L.y + R.y) / 2;
  return {
    yaw:   (nose.x - midX) / span,
    pitch: (nose.y - midY) / span - (chin.y - brow.y) / span * 0.25,
  };
}

function classify(res) {
  if (!res || !res.faceLandmarks || res.faceLandmarks.length === 0) {
    return { state: "absent", conf: 0.9, yaw: null, pitch: null, eyes: null };
  }
  const lm = res.faceLandmarks[0];
  let { yaw, pitch } = pose(lm);
  if (centre) { yaw -= centre.yaw; pitch -= centre.pitch; }

  // Gaze, from the ARKit-standard blendshapes the model already returns.
  //
  // Head pose alone misses the case that matters most here: glancing at a
  // phone on the desk moves the eyes far more than the head. These scores are
  // an explicit eye-direction signal, which is a stronger reason to call
  // someone distracted than a few degrees of head rotation.
  let closed = 0, gaze = 0;
  const bs = res.faceBlendshapes?.[0]?.categories;
  if (bs) {
    const g = n => bs.find(c => c.categoryName === n)?.score ?? 0;
    closed = Math.max(g("eyeBlinkLeft"), g("eyeBlinkRight"));
    gaze = Math.max(
      g("eyeLookOutLeft"),  g("eyeLookOutRight"),
      g("eyeLookInLeft"),   g("eyeLookInRight"),
      g("eyeLookDownLeft"), g("eyeLookDownRight"));
  }
  lastGaze = gaze;
  const gazeRel = centre ? Math.max(0, gaze - centre.gaze) : gaze;

  const headOff = Math.abs(yaw) > YAW_LIMIT || Math.abs(pitch) > PITCH_LIMIT;
  const eyesOff = gazeRel > GAZE_LIMIT;

  // Eyes shut is not distraction. A blink is 100-400 ms and the window vote
  // absorbs it; sustained closure is drowsiness, a different signal this
  // project does not claim to detect. So closure is reported, never scored.
  const state = (headOff || eyesOff) ? "distracted" : "focused";
  const conf = Math.min(1, Math.max(0.5,
    1 - Math.max(Math.abs(yaw) / YAW_LIMIT,
                 Math.abs(pitch) / PITCH_LIMIT,
                 gazeRel / GAZE_LIMIT) * 0.3));
  return { state, conf, yaw, pitch, eyes: closed, gaze: gazeRel };
}

$("cal").onclick = () => {
  if (!lastLm) return;
  centre = pose(lastLm);
  centre.gaze = lastGaze;
  $("err").textContent = "Centre calibrated.";
};

let lastLm = null;

function tick() {
  if (!landmarker || !img.naturalWidth) return;
  cv.width = img.naturalWidth; cv.height = img.naturalHeight;
  ctx.drawImage(img, 0, 0);

  let res;
  const t0 = performance.now();
  try { res = landmarker.detect(cv); }
  catch (e) { $("err").textContent = "detect: " + e.message; return; }
  const dt = performance.now() - t0;

  tickCount++;
  const r = classify(res);
  lastLm = res?.faceLandmarks?.[0] ?? null;
  // Classification always runs, so the readouts stay live and calibration
  // works between sessions. Only the vote is gated — the vote is the thing
  // that eventually becomes a row in focus_samples.
  if (sampling) votes.push(r.state);

  $("state").textContent = r.state;
  $("state").className = "state " + r.state;
  $("yaw").textContent   = r.yaw   === null ? "-" : r.yaw.toFixed(3);
  $("pitch").textContent = r.pitch === null ? "-" : r.pitch.toFixed(3);
  $("gaze").textContent  = r.gaze  === undefined || r.gaze === null ? "-" : r.gaze.toFixed(2);
  $("eyes").textContent  = r.eyes  === null ? "-" : r.eyes.toFixed(2);
  $("ms").textContent    = dt.toFixed(0) + " ms";

  // Measured, not assumed. Background this tab for a few minutes and come
  // back: if this collapsed, the audio exemption is not holding.
  const now = performance.now();
  if (now - rateAt >= 5000) {
    const hz = tickCount / ((now - rateAt) / 1000);
    $("rate").textContent = hz.toFixed(2) + "/s" + (document.hidden ? " (hidden)" : "");
    tickCount = 0; rateAt = now;
  }

  if (lastLm) {
    ctx.fillStyle = r.state === "focused" ? "#0ca30c" : "#fab219";
    for (const i of [33, 263, 1, 168, 152]) {
      ctx.beginPath();
      ctx.arc(lastLm[i].x * cv.width, lastLm[i].y * cv.height, 3, 0, 7);
      ctx.fill();
    }
  }
}

// One sample per window, decided by majority vote over the frames in it. A
// single frame should never decide a focus state: a blink or a glance is not
// distraction, and the vote is what makes those transients disappear.
async function flush() {
  if (votes.length === 0) return;
  const tally = {};
  for (const v of votes) tally[v] = (tally[v] ?? 0) + 1;
  const state = Object.keys(tally).reduce((a, b) => tally[a] >= tally[b] ? a : b);
  const conf = tally[state] / votes.length;
  votes.length = 0;

  pending.push({ ts: new Date().toISOString(), state, confidence: conf });
}

// Post in batches rather than one sample at a time.
//
// Every POST costs the board a TLS handshake, and one per 2 s window was
// slower than the windows arrived — its queue backed up and dropped samples,
// which is why roughly a third went missing. The ingest route takes up to 500
// samples per call and always did; it was built for this.
async function post() {
  if (pending.length === 0) return;
  const batch = pending.splice(0, 100);
  try {
    const r = await fetch("/focus", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ samples: batch }),
    });
    $("post").textContent = r.ok ? "ok" : "HTTP " + r.status;
    if (r.ok) { sent += batch.length; $("sent").textContent = sent; }
    // A failed batch goes back to the front, so a transient error costs a
    // delay rather than the samples themselves.
    else pending.unshift(...batch);
  } catch (e) {
    $("post").textContent = "failed";
    pending.unshift(...batch);
  }
}

// Idle ticks at 1 Hz rather than the full rate: the preview and the readouts
// stay live so calibration works before a session starts, without spending GPU
// on inference nothing is recording. A setInterval cannot change its own
// period, so this reschedules itself instead.
function tickLoop() {
  tick();
  setTimeout(tickLoop, sampling ? Math.max(60, 1000 / HZ) : 1000);
}
tickLoop();

// Only the window timer is gated. `post` keeps its own schedule so the last
// batch of a finished session still goes up, and it returns immediately when
// there is nothing pending.
setInterval(() => { if (sampling) flush(); }, WINDOW_MS);
setInterval(post, POST_MS);

// Ask once immediately — otherwise pressing the button and watching this page
// means staring at "checking" for a poll interval, which reads as a failure.
pollSession();
setInterval(pollSession, SESS_MS);
</script>
)HTMLPAGE";
