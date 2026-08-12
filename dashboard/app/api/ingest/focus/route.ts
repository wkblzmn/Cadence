import { NextResponse } from "next/server";
import { sql } from "@/lib/db";
import { checkAuth, reqString, reqTimestamp, reqEnum, optNumber, badRequest, BadRequest } from "@/lib/auth";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const STATES = ["focused", "distracted", "absent"] as const;

// Spec §7
// POST { device_id, samples: [{ ts, state, confidence }] } — batched
//
// Written now because it is part of the locked contract, but nothing calls it
// until Phase 4. The phone posts these, not the hub.

export async function POST(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const b = await req.json();
    const deviceId = reqString(b.device_id, "device_id", 64);

    if (!Array.isArray(b.samples) || b.samples.length === 0 || b.samples.length > 500) {
      throw new BadRequest("samples must be an array of 1-500 items");
    }

    const rows = b.samples.map((s: Record<string, unknown>, i: number) => {
      try {
        return {
          ts:         reqTimestamp(s.ts, "ts"),
          state:      reqEnum(s.state, "state", STATES),
          confidence: optNumber(s.confidence, "confidence", 0, 1),
        };
      } catch (err) {
        throw new BadRequest(
          `samples[${i}]: ${err instanceof Error ? err.message : "invalid"}`
        );
      }
    });

    await sql`
      insert into focus_samples (device_id, ts, state, confidence)
      select ${deviceId}, t.ts, t.state, t.confidence
      from unnest(
        ${rows.map(r => r.ts)}::timestamptz[],
        ${rows.map(r => r.state)}::text[],
        ${rows.map(r => r.confidence)}::real[]
      ) as t(ts, state, confidence)
    `;

    return NextResponse.json({ ok: true, received: rows.length }, { status: 201 });
  } catch (e) {
    return badRequest(e);
  }
}