import { NextResponse } from "next/server";
import { sql } from "@/lib/db";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// GET /api/stats/hub — everything the hub's Home page draws, in one response.
//
// Why this exists next to /api/stats/daily rather than extending it: the two
// have different consumers and therefore different shapes. /api/stats/daily is
// the general-purpose route — it takes an arbitrary range and returns labelled
// objects. This one is shaped for a device with a hand-rolled parser and one
// TLS handshake to spend:
//
//   * `days` is a flat array of 7 integers, oldest first, not 7 objects. The
//     hub walks it with a number scanner; objects would need a real parser.
//   * `ratio` is an integer percent, not a float. No float parsing on device.
//   * `attention.data` is 1/0 rather than a boolean, for the same reason.
//   * The week and today's attention arrive together, because two fetches
//     would mean two TLS handshakes on a board where the handshake is the
//     most expensive thing it does.
//
// Every number is still computed in Postgres. §4 is explicit that the device
// emits events and never totals, and that applies to this route too: it
// aggregates nothing of its own.
//
// Unauthenticated GET, matching /api/stats/daily and GET /api/todos — reads
// are open on this deployment, writes carry DEVICE_TOKEN. See the exposure
// note in the project's task board before changing that.

const DEVICE_ID = process.env.DEVICE_ID ?? "cadence-hub-01";
const TZ        = process.env.DISPLAY_TZ ?? "Asia/Dhaka";

const DAYS = 7;

function num(v: unknown): number {
  return v === null || v === undefined ? 0 : Number(v);
}

export async function GET(req: Request) {
  const deviceId = new URL(req.url).searchParams.get("device_id") ?? DEVICE_ID;

  // The day window is resolved in Postgres, not here. Asking JS for "today in
  // Asia/Dhaka" means trusting the server process timezone, which is UTC on
  // Vercel and lands a day early for six hours of every day.
  const [todayRows, weekRows, attnRows] = await Promise.all([
    sql`select (now() at time zone ${TZ})::date::text as today`,
    sql`
      select day::text as day, focus_seconds
      from session_daily(
        ${deviceId},
        -- ::int is load-bearing. The driver binds this as an untyped
        -- parameter, and Postgres then resolves date-minus-parameter to the
        -- date-minus-date overload that returns integer -- so session_daily()
        -- gets an integer where it wants a date, and no such function exists.
        -- The dashboard's copy of this query only escapes it by hardcoding 6.
        ((now() at time zone ${TZ})::date - ${DAYS - 1}::int),
         (now() at time zone ${TZ})::date,
        ${TZ}
      )`,
    sql`
      select focused_seconds, distracted_seconds, absent_seconds,
             longest_focus_s, distraction_events, focus_ratio
      from focus_daily(
        ${deviceId},
        (now() at time zone ${TZ})::date,
        (now() at time zone ${TZ})::date,
        ${TZ}
      )`,
  ]);

  const today = String(todayRows[0].today);

  // Day keys as strings end to end. Parsing a bare `date` into a JS Date is
  // what made /api/stats/daily report zeros against a populated table — the
  // driver resolves it in the process timezone. Same trap, same avoidance.
  const keys: string[] = [];
  const d = new Date(`${today}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() - (DAYS - 1));
  for (let i = 0; i < DAYS; i++) {
    keys.push(d.toISOString().slice(0, 10));
    d.setUTCDate(d.getUTCDate() + 1);
  }

  const byDay = new Map(weekRows.map((r) => [String(r.day), num(r.focus_seconds)]));

  // Gap-filled and fixed-length. The hub reads exactly DAYS integers and
  // indexes the last one as today; a short array would silently shift the
  // whole chart by a day.
  const days = keys.map((k) => byDay.get(k) ?? 0);

  const a = attnRows[0] ?? null;
  const focused    = a ? num(a.focused_seconds)    : 0;
  const distracted = a ? num(a.distracted_seconds) : 0;
  const away       = a ? num(a.absent_seconds)     : 0;

  return NextResponse.json(
    {
      today,
      tz: TZ,
      days,
      today_seconds: days[DAYS - 1],
      peak_seconds: Math.max(...days),
      week_seconds: days.reduce((x, y) => x + y, 0),
      attention: {
        // 0 means "the vision page has not run today", which is the common
        // case and is not the same as a day of zero focus. The hub draws a
        // different thing for each, so it has to be able to tell them apart.
        data: a && focused + distracted + away > 0 ? 1 : 0,
        focused,
        distracted,
        away,
        longest: a ? num(a.longest_focus_s) : 0,
        breaks: a ? num(a.distraction_events) : 0,
        ratio: a ? Math.round(num(a.focus_ratio) * 100) : 0,
      },
    },
    { headers: { "cache-control": "no-store" } }
  );
}
