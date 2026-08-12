import { NextResponse } from "next/server";
import { sql } from "@/lib/db";
import { checkAuth, reqString, reqTimestamp, reqEnum, badRequest, BadRequest } from "@/lib/auth";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const TYPES  = ["start", "pause", "resume", "stop"] as const;
const SOURCE = ["button", "vision"] as const;

// Spec §7
// POST { device_id, ts, type, source }
//
// Also accepts { device_id, events: [{ ts, type, source }] } so the hub can
// drain its whole queue in one request after a dropped link. Same contract,
// fewer round trips — and the queue exists precisely because links drop.

// req.json() returns `any`, so anything derived from it inherits `any` and
// trips noImplicitAny at build time. Naming the row shape fixes that and
// documents what actually reaches the database.
type EventRow = { ts: string; type: string; source: string };

export async function POST(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const b = await req.json();
    const deviceId = reqString(b.device_id, "device_id", 64);

    const raw: unknown[] = Array.isArray(b.events) ? b.events : [b];
    if (raw.length === 0 || raw.length > 64) {
      throw new BadRequest("events must contain between 1 and 64 items");
    }

    const rows: EventRow[] = raw.map((item, i) => {
      const e = item as Record<string, unknown>;
      try {
        return {
          ts:     reqTimestamp(e.ts, "ts"),
          type:   reqEnum(e.type, "type", TYPES),
          source: reqEnum(e.source ?? "button", "source", SOURCE),
        };
      } catch (err) {
        throw new BadRequest(
          `events[${i}]: ${err instanceof Error ? err.message : "invalid"}`
        );
      }
    });

    // One statement, one round trip. unnest expands the arrays into rows;
    // ON CONFLICT makes the whole batch safely retryable.
    const inserted = await sql`
      insert into session_events (device_id, ts, type, source)
      select ${deviceId}, t.ts, t.type, t.source
      from unnest(
        ${rows.map(r => r.ts)}::timestamptz[],
        ${rows.map(r => r.type)}::text[],
        ${rows.map(r => r.source)}::text[]
      ) as t(ts, type, source)
      on conflict (device_id, ts, type) do nothing
      returning id
    `;

    return NextResponse.json(
      { ok: true, received: rows.length, inserted: inserted.length },
      { status: 201 }
    );
  } catch (e) {
    return badRequest(e);
  }
}