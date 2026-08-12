import { sql } from "@/lib/db";

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

async function load() {
  const [todayRows, dailyRows, stretchRows, readingRows, todoRows] =
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
        select id, title
        from todos
        where done = false
        order by position asc, updated_at asc
        limit 12`,
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
    todos: todoRows as Array<{ id: string; title: string }>,
  };
}

// ── chart ────────────────────────────────────────────────────────────────
//
// Single series, so no legend: the card title says what is plotted. One hue
// for every column — a value-ramp here would double-encode height as darkness
// and burn the only free channel on information the bar already carries.

const W = 700;
const H = 200;
const PAD_L = 46;
const PAD_R = 12;
const PAD_T = 24; // headroom for the cap labels
const AXIS_Y = 158;
const LABEL_Y = 180;

function niceCeil(seconds: number): number {
  const mins = seconds / 60;
  for (const s of [15, 30, 60, 90, 120, 180, 240, 300, 360, 480, 600, 720]) {
    if (mins <= s) return s * 60;
  }
  return Math.ceil(mins / 60) * 3600;
}

// Rounded at the data end, square at the baseline.
function barPath(x: number, y: number, w: number, h: number): string {
  const r = Math.min(4, h);
  return [
    `M ${x} ${y + h}`,
    `L ${x} ${y + r}`,
    `Q ${x} ${y} ${x + r} ${y}`,
    `L ${x + w - r} ${y}`,
    `Q ${x + w} ${y} ${x + w} ${y + r}`,
    `L ${x + w} ${y + h}`,
    "Z",
  ].join(" ");
}

function FocusChart({ days, today }: { days: Day[]; today: string }) {
  const plotW = W - PAD_L - PAD_R;
  const plotH = AXIS_Y - PAD_T;
  const band = plotW / days.length;
  const barW = Math.min(24, band - 16);

  const peak = Math.max(...days.map((d) => d.focus_seconds));
  const yMax = niceCeil(Math.max(peak, 1));
  const ticks = [0, yMax / 2, yMax];

  // Label selectively: today, and the peak day when it is a different column.
  const peakIdx = days.findIndex((d) => d.focus_seconds === peak && peak > 0);
  const todayIdx = days.findIndex((d) => d.day === today);
  const labelled = new Set([peakIdx, todayIdx].filter((i) => i >= 0));

  return (
    <svg
      viewBox={`0 0 ${W} ${H}`}
      className="w-full h-auto"
      role="img"
      aria-label={`Focus time per day for the last 7 days, ending ${today}. Peak ${fmtDuration(peak)}.`}
    >
      {ticks.map((t) => {
        const y = AXIS_Y - (t / yMax) * plotH;
        return (
          <g key={t}>
            <line
              x1={PAD_L}
              x2={W - PAD_R}
              y1={y}
              y2={y}
              stroke={t === 0 ? "var(--baseline)" : "var(--gridline)"}
              strokeWidth="1"
            />
            <text
              x={PAD_L - 10}
              y={y + 4}
              textAnchor="end"
              fontSize="11"
              fill="var(--text-muted)"
              style={{ fontVariantNumeric: "tabular-nums" }}
            >
              {t === 0 ? "0" : fmtDuration(t)}
            </text>
          </g>
        );
      })}

      {days.map((d, i) => {
        const h = (d.focus_seconds / yMax) * plotH;
        const x = PAD_L + i * band + (band - barW) / 2;
        const y = AXIS_Y - h;
        const isToday = d.day === today;

        return (
          <g key={d.day}>
            <title>
              {`${weekday(d.day)} ${d.day} — ${fmtDuration(d.focus_seconds)}, ${d.session_count} segment${d.session_count === 1 ? "" : "s"}`}
            </title>

            {/* Hit target spans the whole band so hover is not pixel-hunting. */}
            <rect
              x={PAD_L + i * band}
              y={PAD_T}
              width={band}
              height={AXIS_Y - PAD_T}
              fill="transparent"
            />

            {d.focus_seconds > 0 && (
              <path d={barPath(x, y, barW, h)} fill="var(--series-1)" />
            )}

            {labelled.has(i) && d.focus_seconds > 0 && (
              <text
                x={x + barW / 2}
                y={y - 8}
                textAnchor="middle"
                fontSize="11"
                fontWeight="600"
                fill="var(--text-secondary)"
              >
                {fmtDuration(d.focus_seconds)}
              </text>
            )}

            <text
              x={PAD_L + i * band + band / 2}
              y={LABEL_Y}
              textAnchor="middle"
              fontSize="11"
              fill={isToday ? "var(--text-secondary)" : "var(--text-muted)"}
              fontWeight={isToday ? 600 : 400}
            >
              {weekday(d.day)}
            </text>
          </g>
        );
      })}
    </svg>
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
      className={`rounded-xl border p-5 ${className}`}
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

// ── page ─────────────────────────────────────────────────────────────────

export default async function Home() {
  const { today, days, longestToday, reading, todos } = await load();

  const todayRow = days.find((d) => d.day === today);
  const todaySeconds = todayRow?.focus_seconds ?? 0;
  const weekSeconds = days.reduce((a, d) => a + d.focus_seconds, 0);
  const age = reading ? num(reading.age_seconds) : null;
  const val = (k: string): number | null =>
    reading && reading[k] !== null ? Number(reading[k]) : null;

  return (
    <main className="mx-auto w-full max-w-5xl px-6 py-10">
      <header className="mb-8 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h1 className="text-xl font-semibold tracking-tight">Cadence</h1>
          <p className="text-sm" style={{ color: "var(--text-muted)" }}>
            {DEVICE_ID} · {TZ} · {today}
          </p>
        </div>
        <StatusChip ageSeconds={age} />
      </header>

      {/* Hero — the one number the view leads with. */}
      <Card className="mb-5">
        <div className="text-xs" style={{ color: "var(--text-muted)" }}>
          Focused today
        </div>
        <div className="mt-1 text-6xl font-semibold tracking-tight">
          {fmtDuration(todaySeconds)}
        </div>
        <div className="mt-6 grid grid-cols-2 gap-6 sm:grid-cols-3">
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
          {todos.length === 0 ? (
            <p className="text-sm" style={{ color: "var(--text-muted)" }}>
              Nothing open. Add one with{" "}
              <code className="text-xs">POST /api/todos</code>.
            </p>
          ) : (
            <ul className="space-y-2">
              {todos.map((t) => (
                <li key={t.id} className="flex items-start gap-2 text-sm">
                  <span aria-hidden style={{ color: "var(--text-muted)" }}>
                    ○
                  </span>
                  <span>{t.title}</span>
                </li>
              ))}
            </ul>
          )}
        </Card>
      </div>
    </main>
  );
}
