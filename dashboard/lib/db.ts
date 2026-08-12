import { neon } from "@neondatabase/serverless";

if (!process.env.DATABASE_URL) {
  throw new Error("DATABASE_URL is not set. Copy .env.local.example to .env.local.");
}

// HTTP driver, not TCP. One request per query, no pool to exhaust — which is
// what you want in serverless functions that may cold-start in parallel.
// Use the POOLED connection string (hostname contains "-pooler").
export const sql = neon(process.env.DATABASE_URL);