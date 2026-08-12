import { NextResponse } from "next/server";

// Spec §7: a static bearer token stored in firmware. Not real security —
// enough to keep the endpoint from being open to the internet.

const TOKEN = process.env.DEVICE_TOKEN ?? "";

// Constant-time compare. A naive === leaks the token one character at a time
// through response timing. Cheap to do properly, so do it properly.
function safeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

export function checkAuth(req: Request): NextResponse | null {
  if (!TOKEN) {
    console.error("DEVICE_TOKEN is not set — refusing all ingest requests.");
    return NextResponse.json({ error: "server misconfigured" }, { status: 500 });
  }

  const header = req.headers.get("authorization") ?? "";
  const presented = header.startsWith("Bearer ") ? header.slice(7).trim() : "";

  if (!safeEqual(presented, TOKEN)) {
    return NextResponse.json({ error: "unauthorized" }, { status: 401 });
  }
  return null; // null means "carry on"
}

// ── validation helpers ───────────────────────────────────────────────────
// Hand-rolled rather than a schema library: one less dependency, and every
// rule is visible in the route that enforces it.

export class BadRequest extends Error {}

export function reqString(v: unknown, field: string, max = 128): string {
  if (typeof v !== "string" || v.length === 0 || v.length > max) {
    throw new BadRequest(`${field} must be a string of 1-${max} chars`);
  }
  return v;
}

export function reqEnum<T extends string>(
  v: unknown, field: string, allowed: readonly T[]
): T {
  if (typeof v !== "string" || !allowed.includes(v as T)) {
    throw new BadRequest(`${field} must be one of: ${allowed.join(", ")}`);
  }
  return v as T;
}

// ISO 8601 only. A timestamp the hub could not have produced is a bug
// somewhere, and silently coercing it would hide that bug in the data.
export function reqTimestamp(v: unknown, field: string): string {
  if (typeof v !== "string") throw new BadRequest(`${field} must be an ISO 8601 string`);
  const t = Date.parse(v);
  if (Number.isNaN(t)) throw new BadRequest(`${field} is not a valid timestamp`);

  const skewMs = Math.abs(Date.now() - t);
  if (skewMs > 7 * 24 * 3600 * 1000) {
    throw new BadRequest(`${field} is more than 7 days from now — check NTP on the device`);
  }
  return new Date(t).toISOString();
}

// Sensor fields are nullable by design: the environment page degrades to "--"
// when the BME280 is absent, and the reading is still worth recording for lux.
export function optNumber(v: unknown, field: string, min: number, max: number): number | null {
  if (v === null || v === undefined) return null;
  if (typeof v !== "number" || !Number.isFinite(v)) {
    throw new BadRequest(`${field} must be a number or null`);
  }
  if (v < min || v > max) throw new BadRequest(`${field} out of range (${min}..${max})`);
  return v;
}

export function badRequest(e: unknown): NextResponse {
  const msg = e instanceof BadRequest ? e.message : "invalid request body";
  if (!(e instanceof BadRequest)) console.error(e);
  return NextResponse.json({ error: msg }, { status: 400 });
}