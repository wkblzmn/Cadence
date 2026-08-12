import { NextResponse } from "next/server";
import { sql } from "@/lib/db";
import { checkAuth, reqString, reqTimestamp, optNumber, badRequest } from "@/lib/auth";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// Spec §7
// POST { device_id, ts, temp_c, humidity, pressure_hpa, lux, real_feel_c }

export async function POST(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const b = await req.json();

    const deviceId = reqString(b.device_id, "device_id", 64);
    const ts       = reqTimestamp(b.ts, "ts");

    // Ranges are physical sanity checks, not calibration. A BME280 reporting
    // 200 C is a bus fault, and it should be rejected here rather than
    // silently skewing the dashboard's axes.
    const tempC     = optNumber(b.temp_c,       "temp_c",       -50, 100);
    const humidity  = optNumber(b.humidity,     "humidity",       0, 100);
    const pressure  = optNumber(b.pressure_hpa, "pressure_hpa", 300, 1200);
    const lux       = optNumber(b.lux,          "lux",            0, 200000);
    const realFeel  = optNumber(b.real_feel_c,  "real_feel_c",  -50, 100);

    // ON CONFLICT makes a retried POST a no-op instead of a duplicate row.
    await sql`
      insert into readings (device_id, ts, temp_c, humidity, pressure_hpa, lux, real_feel_c)
      values (${deviceId}, ${ts}, ${tempC}, ${humidity}, ${pressure}, ${lux}, ${realFeel})
      on conflict (device_id, ts) do nothing
    `;

    return NextResponse.json({ ok: true }, { status: 201 });
  } catch (e) {
    return badRequest(e);
  }
}