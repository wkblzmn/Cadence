import { sql } from "@/lib/db";
import Tasks, { type Todo } from "./tasks";
import ThemeToggle from "./theme-toggle";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const DEVICE_ID = process.env.DEVICE_ID ?? "cadence-hub-01";
const TZ = process.env.DISPLAY_TZ ?? "Asia/Dhaka";

// ── helpers ──────────────────────────────────────────────────────────────

// Every number the device produces is seconds. Nothing here accumulates
// anything of its own (spec §4) — it only formats what the fold returned.
function fmtDuration(sec: number): string {
  if (sec <= 0) return "0m";
  if (sec < 60) return `${sec}s`;
  const h = Math.floor(sec / 3600);
  const m = Math.round((sec - h * 3600) / 60);
  if (h && m) return `${h}h ${m}m`;
  if (h) return `${h}h`;
  return `${m}m`;
}

// Day keys are handled as strings from end to end. Parsing them into a Date
// and formatting it back is what made /api/stats/daily report zeros: the
// driver resolves a bare `date` in the process timezone and lands a day early.
function dayKeys(today: string, n: number): string[] {
  const out: string[] = [];
  const d = new Date(`${today}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() - (n - 1));
  for (let i = 0; i < n; i++) {
    out.push(d.toISOString().slice(0, 10));
    d.setUTCDate(d.getUTCDate() + 1);
  }
  return out;
}

function weekday(key: string): string {
  return new Date(`${key}T00:00:00Z`).toLocaleDateString("en-US", {
    weekday: "short",
    timeZone: "UTC",
  });
}

function num(v: unknown): number {
  return v === null || v === undefined ? 0 : Number(v);
}

// ── data ─────────────────────────────────────────────────────────────────

type Day = { day: string; focus_seconds: number; session_count: number };

type Attention = {
  focused: number;
  distracted: number;
  absent: number;
  longest: number;
  breaks: number;
  ratio: number;
};

async function load() {
  const [todayRows, dailyRows, stretchRows, readingRows, todoRows, focusRows] =
    await Promise.all([
      sql`select (now() at time zone ${TZ})::date::text as today`,
      sql`
        select day::text as day, focus_seconds, session_count
        from session_daily(
          ${DEVICE_ID},
          ((now() at time zone ${TZ})::date - 6),
           (now() at time zone ${TZ})::date,
          ${TZ}
        )`,
      sql`
        select longest_seconds
        from session_longest_stretch
        where device_id = ${DEVICE_ID}
          and day = (now() at time zone ${TZ})::date`,
      sql`
        select ts::text as ts, temp_c, humidity, pressure_hpa, lux, real_feel_c,
               extract(epoch from (now() - ts))::int as age_seconds
        from readings_latest
        where device_id = ${DEVICE_ID}`,
      sql`
        select id, title, position, done
        from todos
        order by done asc, position asc, updated_at asc
        limit 200`,
      // day::text for the same reason as everywhere else — a bare date comes
      // back as a JS Date resolved in the server's timezone.
      sql`
        select day::text as day, focused_seconds, distracted_seconds,
               absent_seconds, longest_focus_s, distraction_events, focus_ratio
        from focus_daily(
          ${DEVICE_ID},
          (now() at time zone ${TZ})::date,
          (now() at time zone ${TZ})::date,
          ${TZ}
        )`,
    ]);

  const today = String(todayRows[0].today);
  const byDay = new Map(dailyRows.map((r) => [String(r.day), r]));

  // Gap-fill so an idle day is a visible zero rather than a missing column.
  const days: Day[] = dayKeys(today, 7).map((key) => {
    const hit = byDay.get(key);
    return {
      day: key,
      focus_seconds: num(hit?.focus_seconds),
      session_count: num(hit?.session_count),
    };
  });

  return {
    today,
    days,
    longestToday: num(stretchRows[0]?.longest_seconds),
    reading: readingRows[0] ?? null,
    // Null when the vision page has not run today. That is the honest state:
    // it is a calibration tool, not an always-on tracker, so most days will
    // have no attention data at all and the card says so rather than
    // rendering an empty chart.
    attention: focusRows[0]
      ? ({
          focused: num(focusRows[0].focused_seconds),
          distracted: num(focusRows[0].distracted_seconds),
          absent: num(focusRows[0].absent_seconds),
          longest: num(focusRows[0].longest_focus_s),
          breaks: num(focusRows[0].distraction_events),
          ratio: num(focusRows[0].focus_ratio),
        } as Attention)
      : null,
    todos: todoRows.map<Todo>((r) => ({
      id: String(r.id),
      title: String(r.title),
      position: num(r.position),
      done: Boolean(r.done),
    })),
  };
}

// ── chart ────────────────────────────────────────────────────────────────
//
// Plain HTML and CSS, not SVG. A viewBox scales its text along with its
// geometry, so the 11px axis labels came out around 6px on a 375px phone —
// fine in the desktop mock, useless on the device. Laying the columns out
// with flex keeps every label at a real CSS size at any width while the bars
// still scale.
//
// Single series, so no legend: the card title says what is plotted. One hue
// for every column — a value-ramp here would double-encode height as darkness
// and burn the only free channel on information the bar already carries.

// Strictly greater, not >=, so the tallest bar never reaches 100% of the plot.
// A bar at exactly full height would push its own value label out of the plot
// area and into the card title.
function niceCeil(seconds: number): number {
  const mins = seconds / 60;
  for (const s of [15, 30, 60, 90, 120, 180, 240, 300, 360, 480, 600, 720]) {
    if (mins < s) return s * 60;
  }
  return Math.ceil(mins / 60) * 3600;
}

function FocusChart({ days, today }: { days: Day[]; today: string }) {
  const peak = Math.max(...days.map((d) => d.focus_seconds));
  const yMax = niceCeil(Math.max(peak, 1));
  const ticks = [yMax, yMax / 2, 0];

  // Label selectively: today, and the peak day when it is a different column.
  const peakIdx = days.findIndex((d) => d.focus_seconds === peak && peak > 0);
  const todayIdx = days.findIndex((d) => d.day === today);
  const labelled = new Set([peakIdx, todayIdx].filter((i) => i >= 0));

  const plotH = "h-40 sm:h-48";
  const gutter = "w-9 shrink-0 sm:w-11";

  return (
    <div
      role="img"
      aria-label={`Focus time per day for the last 7 days, ending ${today}. Peak ${fmtDuration(peak)}.`}
    >
      <div className="flex gap-2">
        {/* Ticks are positioned from the same percentage as their gridline
            rather than spaced evenly, so the two cannot drift apart. */}
        <div className={`relative ${gutter} ${plotH}`}>
          {ticks.map((t) => (
            <span
              key={t}
              className="absolute right-0 translate-y-1/2 text-[10px] sm:text-[11px]"
              style={{
                bottom: `${(t / yMax) * 100}%`,
                color: "var(--text-muted)",
                fontVariantNumeric: "tabular-nums",
              }}
            >
              {t === 0 ? "0" : fmtDuration(t)}
            </span>
          ))}
        </div>

        <div className={`relative flex-1 ${plotH}`}>
          {ticks.map((t) => (
            <div
              key={t}
              className="absolute inset-x-0 h-px"
              style={{
                bottom: `${(t / yMax) * 100}%`,
                background: t === 0 ? "var(--baseline)" : "var(--gridline)",
              }}
            />
          ))}

          <div className="absolute inset-0 flex items-end">
            {days.map((d, i) => {
              const pct = (d.focus_seconds / yMax) * 100;
              return (
                // The hover target is the whole column, not the bar, so a
                // near-empty day is not a pixel hunt.
                <div
                  key={d.day}
                  className="relative h-full flex-1"
                  title={`${weekday(d.day)} ${d.day} — ${fmtDuration(d.focus_seconds)}, ${d.session_count} segment${d.session_count === 1 ? "" : "s"}`}
                >
                  {d.focus_seconds > 0 && (
                    <div
                      className="absolute bottom-0 left-1/2 w-6 max-w-[60%] -translate-x-1/2"
                      style={{
                        height: `${pct}%`,
                        background: "var(--series-1)",
                        borderRadius: "4px 4px 0 0",
                      }}
                    />
                  )}

                  {labelled.has(i) && d.focus_seconds > 0 && (
                    <span
                      className="absolute left-1/2 -translate-x-1/2 whitespace-nowrap text-[10px] font-semibold sm:text-[11px]"
                      style={{
                        bottom: `calc(${pct}% + 4px)`,
                        color: "var(--text-secondary)",
                      }}
                    >
                      {fmtDuration(d.focus_seconds)}
                    </span>
                  )}
                </div>
              );
            })}
          </div>
        </div>
      </div>

      <div className="mt-2 flex gap-2">
        <div className={gutter} />
        <div className="flex flex-1">
          {days.map((d) => (
            <div
              key={d.day}
              className="flex-1 text-center text-[10px] sm:text-[11px]"
              style={{
                color:
                  d.day === today ? "var(--text-secondary)" : "var(--text-muted)",
                fontWeight: d.day === today ? 600 : 400,
              }}
            >
              {weekday(d.day)}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

// ── pieces ───────────────────────────────────────────────────────────────

function Card({
  title,
  children,
  className = "",
}: {
  title?: string;
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <section
      className={`rounded-xl border p-4 sm:p-5 ${className}`}
      style={{
        background: "var(--surface-1)",
        borderColor: "var(--hairline)",
      }}
    >
      {title && (
        <h2
          className="mb-4 text-sm font-semibold"
          style={{ color: "var(--text-secondary)" }}
        >
          {title}
        </h2>
      )}
      {children}
    </section>
  );
}

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <div className="text-xs" style={{ color: "var(--text-muted)" }}>
        {label}
      </div>
      <div
        className="mt-1 text-2xl font-semibold"
        style={{ color: "var(--text-primary)" }}
      >
        {value}
      </div>
    </div>
  );
}

// Status never rides on colour alone — every state ships a glyph and a word.
function StatusChip({ ageSeconds }: { ageSeconds: number | null }) {
  const state =
    ageSeconds === null
      ? { c: "var(--status-critical)", g: "■", t: "No data" }
      : ageSeconds < 180
        ? { c: "var(--status-good)", g: "●", t: "Live" }
        : ageSeconds < 900
          ? { c: "var(--status-warning)", g: "▲", t: "Delayed" }
          : { c: "var(--status-critical)", g: "■", t: "Offline" };

  return (
    <span
      className="inline-flex items-center gap-2 rounded-full border px-3 py-1 text-xs font-medium"
      style={{ borderColor: "var(--hairline)", color: "var(--text-secondary)" }}
    >
      <span aria-hidden style={{ color: state.c }}>
        {state.g}
      </span>
      {state.t}
      {ageSeconds !== null && (
        <span style={{ color: "var(--text-muted)" }}>
          {ageSeconds < 120
            ? `${ageSeconds}s ago`
            : `${Math.round(ageSeconds / 60)}m ago`}
        </span>
      )}
    </span>
  );
}

function EnvRow({
  label,
  value,
  unit,
}: {
  label: string;
  value: number | null;
  unit: string;
}) {
  return (
    <div
      className="flex items-baseline justify-between border-b py-2 last:border-b-0"
      style={{ borderColor: "var(--hairline)" }}
    >
      <span className="text-sm" style={{ color: "var(--text-secondary)" }}>
        {label}
      </span>
      <span
        className="text-sm font-medium"
        style={{
          color: value === null ? "var(--text-muted)" : "var(--text-primary)",
          fontVariantNumeric: "tabular-nums",
        }}
      >
        {value === null ? "—" : `${value.toFixed(1)} ${unit}`}
      </span>
    </div>
  );
}

// Part-to-whole across three states, so a single stacked bar rather than
// three. These carry the status palette rather than categorical hues because
// they genuinely are states with a valence — and each ships with a label, so
// colour is never the only channel.
function AttentionCard({ a }: { a: Attention | null }) {
  if (!a || a.focused + a.distracted + a.absent === 0) {
    return (
      <p className="text-sm" style={{ color: "var(--text-muted)" }}>
        No attention data today. It is recorded while the camera board&apos;s{" "}
        <code className="text-xs">/vision</code> page is open.
      </p>
    );
  }

  const total = a.focused + a.distracted + a.absent;
  const parts = [
    { k: "Focused", v: a.focused, c: "var(--status-good)" },
    { k: "Distracted", v: a.distracted, c: "var(--status-warning)" },
    { k: "Away", v: a.absent, c: "var(--text-muted)" },
  ].filter((p) => p.v > 0);

  return (
    <div>
      <div className="grid grid-cols-3 gap-4">
        <Stat label="Focus ratio" value={`${Math.round(a.ratio * 100)}%`} />
        <Stat label="Longest focus" value={fmtDuration(a.longest)} />
        <Stat label="Breaks" value={String(a.breaks)} />
      </div>

      {/* 2px surface gaps separate the segments — a gap, not a border. */}
      <div className="mt-4 flex h-2.5 w-full overflow-hidden rounded-full">
        {parts.map((p, i) => (
          <div
            key={p.k}
            title={`${p.k} — ${fmtDuration(p.v)}`}
            style={{
              width: `${(p.v / total) * 100}%`,
              background: p.c,
              marginLeft: i === 0 ? 0 : 2,
            }}
          />
        ))}
      </div>

      <div className="mt-3 flex flex-wrap gap-x-4 gap-y-1">
        {parts.map((p) => (
          <span key={p.k} className="flex items-center gap-1.5 text-xs">
            <span
              aria-hidden
              className="inline-block h-2 w-2 rounded-full"
              style={{ background: p.c }}
            />
            <span style={{ color: "var(--text-secondary)" }}>{p.k}</span>
            <span style={{ color: "var(--text-muted)" }}>{fmtDuration(p.v)}</span>
          </span>
        ))}
      </div>

      <p className="mt-3 text-xs" style={{ color: "var(--text-muted)" }}>
        Focus ratio counts focused against distracted, ignoring time away — being
        out of the room is not a lapse in attention.
      </p>
    </div>
  );
}

// ── page ─────────────────────────────────────────────────────────────────

export default async function Home() {
  const { today, days, longestToday, reading, todos, attention } = await load();

  const todayRow = days.find((d) => d.day === today);
  const todaySeconds = todayRow?.focus_seconds ?? 0;
  const weekSeconds = days.reduce((a, d) => a + d.focus_seconds, 0);
  const age = reading ? num(reading.age_seconds) : null;
  const val = (k: string): number | null =>
    reading && reading[k] !== null ? Number(reading[k]) : null;

  return (
    <main className="mx-auto w-full max-w-5xl px-4 py-6 sm:px-6 sm:py-10">
      <header className="mb-6 flex flex-wrap items-center justify-between gap-3 sm:mb-8">
        <div className="min-w-0">
          <h1 className="text-xl font-semibold tracking-tight">Cadence</h1>
          {/* The device line wraps rather than truncating — on a phone it is
              three separate facts, not one string. */}
          <p
            className="text-xs sm:text-sm"
            style={{ color: "var(--text-muted)" }}
          >
            {DEVICE_ID} · {TZ} · {today}
          </p>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <StatusChip ageSeconds={age} />
          <ThemeToggle />
        </div>
      </header>

      {/* Hero — the one number the view leads with. */}
      <Card className="mb-5">
        <div className="text-xs" style={{ color: "var(--text-muted)" }}>
          Focused today
        </div>
        <div className="mt-1 text-5xl font-semibold tracking-tight sm:text-6xl">
          {fmtDuration(todaySeconds)}
        </div>
        <div className="mt-5 grid grid-cols-2 gap-4 sm:mt-6 sm:grid-cols-3 sm:gap-6">
          <Stat label="Longest stretch" value={fmtDuration(longestToday)} />
          <Stat
            label="Segments today"
            value={String(todayRow?.session_count ?? 0)}
          />
          <Stat label="Last 7 days" value={fmtDuration(weekSeconds)} />
        </div>
      </Card>

      <Card title="Focus by day — last 7 days" className="mb-5">
        <FocusChart days={days} today={today} />

        {/* The table twin: every value readable without hover or colour. */}
        <details className="mt-4">
          <summary
            className="cursor-pointer text-xs"
            style={{ color: "var(--text-muted)" }}
          >
            Table view
          </summary>
          <table className="mt-3 w-full text-sm">
            <thead>
              <tr style={{ color: "var(--text-muted)" }}>
                <th className="py-1 text-left font-normal">Day</th>
                <th className="py-1 text-right font-normal">Focus</th>
                <th className="py-1 text-right font-normal">Segments</th>
              </tr>
            </thead>
            <tbody style={{ fontVariantNumeric: "tabular-nums" }}>
              {days.map((d) => (
                <tr
                  key={d.day}
                  className="border-t"
                  style={{ borderColor: "var(--hairline)" }}
                >
                  <td className="py-1.5">
                    {weekday(d.day)} {d.day}
                  </td>
                  <td className="py-1.5 text-right">
                    {fmtDuration(d.focus_seconds)}
                  </td>
                  <td className="py-1.5 text-right">{d.session_count}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </details>
      </Card>

      <Card title="Attention — today" className="mb-5">
        <AttentionCard a={attention} />
      </Card>

      <div className="grid gap-5 md:grid-cols-2">
        <Card title="Environment">
          <EnvRow label="Temperature" value={val("temp_c")} unit="°C" />
          <EnvRow label="Real feel" value={val("real_feel_c")} unit="°C" />
          <EnvRow label="Humidity" value={val("humidity")} unit="%" />
          <EnvRow label="Pressure" value={val("pressure_hpa")} unit="hPa" />
          <EnvRow label="Light" value={val("lux")} unit="lx" />
          {reading && val("temp_c") === null && (
            <p className="mt-3 text-xs" style={{ color: "var(--text-muted)" }}>
              BME280 not on the bus — the hub is reporting light only.
            </p>
          )}
          {!reading && (
            <p className="mt-3 text-xs" style={{ color: "var(--text-muted)" }}>
              No readings yet.
            </p>
          )}
        </Card>

        <Card title="Tasks">
          <Tasks todos={todos} />
        </Card>
      </div>
    </main>
  );
}
