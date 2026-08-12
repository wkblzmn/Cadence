import { NextResponse } from "next/server";
import { sql } from "@/lib/db";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// Spec §7:  GET /api/stats/daily?date=YYYY-MM-DD
// Spec §8:  the session view needs weekly totals too, so a range is supported:
//           GET /api/stats/daily?from=YYYY-MM-DD&to=YYYY-MM-DD
//
// Every number here is computed by session_daily() in the database. §4 is
// explicit that the device emits events and never totals — this route must
// not start accumulating anything of its own.

const DEVICE_ID = process.env.DEVICE_ID ?? "cadence-hub-01";
const TZ        = process.env.DISPLAY_TZ ?? "Asia/Dhaka";

function isDate(s: string | null): s is string {
  return !!s && /^\d{4}-\d{2}-\d{2}$/.test(s) && !Number.isNaN(Date.parse(s));
}

export async function GET(req: Request) {
  const p = new URL(req.url).searchParams;

  const date = p.get("date");
  let from = p.get("from");
  let to   = p.get("to");

  if (isDate(date)) {
    from = date;
    to   = date;
  }

  if (!isDate(from) || !isDate(to)) {
    return NextResponse.json(
      { error: "provide ?date=YYYY-MM-DD or ?from=YYYY-MM-DD&to=YYYY-MM-DD" },
      { status: 400 }
    );
  }
  if (from > to) {
    return NextResponse.json({ error: "from must not be after to" }, { status: 400 });
  }

  const deviceId = p.get("device_id") ?? DEVICE_ID;

  const rows = await sql`
    select day, focus_seconds, session_count
    from session_daily(${deviceId}, ${from}::date, ${to}::date, ${TZ})
  `;

  // Fill gaps so a chart shows an empty day as zero rather than skipping it.
  const byDay = new Map(rows.map(r => [String(r.day).slice(0, 10), r]));
  const days: Array<{ day: string; focus_seconds: number; session_count: number }> = [];

  for (let d = new Date(from + "T00:00:00Z"); ; d.setUTCDate(d.getUTCDate() + 1)) {
    const key = d.toISOString().slice(0, 10);
    const hit = byDay.get(key);
    days.push({
      day: key,
      focus_seconds: hit ? Number(hit.focus_seconds) : 0,
      session_count: hit ? Number(hit.session_count) : 0,
    });
    if (key >= to) break;
  }

  const total = days.reduce((a, d) => a + d.focus_seconds, 0);

  return NextResponse.json(
    {
      device_id: deviceId,
      from,
      to,
      timezone: TZ,
      total_focus_seconds: total,
      days,
    },
    { headers: { "cache-control": "no-store" } }
  );
}