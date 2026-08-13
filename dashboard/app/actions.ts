"use server";

import { revalidatePath } from "next/cache";
import { sql } from "@/lib/db";

// Task mutations for the dashboard.
//
// These are server actions, not calls to /api/todos. The route handlers exist
// for the hub and authenticate with the static bearer token; putting that token
// in the browser to drive the UI would publish it to every visitor. Server
// actions run on the server with the same DATABASE_URL the routes use, which is
// the access model schema.sql part 3 describes: the browser never talks to
// Postgres, and the connection string never leaves the server.
//
// The SQL below deliberately mirrors app/api/todos/route.ts. Both write the
// same rows, so they must agree on position semantics and on the reorder being
// a single statement.

const MAX_TITLE = 200;

function readId(form: FormData): string | null {
  const id = String(form.get("id") ?? "").trim();
  // uuid shape only — anything else would just be a cast error downstream.
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(id)
    ? id
    : null;
}

export async function createTodo(form: FormData): Promise<void> {
  const title = String(form.get("title") ?? "").trim();
  if (!title || title.length > MAX_TITLE) return;

  await sql`
    insert into todos (title, position)
    values (
      ${title},
      (select coalesce(max(position), 0) + 1 from todos)
    )
  `;
  revalidatePath("/");
}

export async function setDone(form: FormData): Promise<void> {
  const id = readId(form);
  if (!id) return;
  const done = String(form.get("done") ?? "") === "true";

  await sql`
    update todos set done = ${done}, updated_at = now()
    where id = ${id}::uuid
  `;
  revalidatePath("/");
}

export async function renameTodo(form: FormData): Promise<void> {
  const id = readId(form);
  const title = String(form.get("title") ?? "").trim();
  if (!id || !title || title.length > MAX_TITLE) return;

  await sql`
    update todos set title = ${title}, updated_at = now()
    where id = ${id}::uuid
  `;
  revalidatePath("/");
}

export async function deleteTodo(form: FormData): Promise<void> {
  const id = readId(form);
  if (!id) return;

  await sql`delete from todos where id = ${id}::uuid`;
  revalidatePath("/");
}

// Move one task past its neighbour. Positions are rewritten as 1..n over the
// open list rather than swapping two numbers: the seed data and the hub's own
// inserts can leave gaps or ties, and a swap through a tie loses the ordering.
export async function moveTodo(form: FormData): Promise<void> {
  const id = readId(form);
  const dir = String(form.get("dir") ?? "");
  if (!id || (dir !== "up" && dir !== "down")) return;

  const rows = await sql`
    select id from todos
    where done = false
    order by position asc, updated_at asc
  `;
  const ids = rows.map((r) => String(r.id));

  const i = ids.indexOf(id);
  if (i < 0) return;
  const j = dir === "up" ? i - 1 : i + 1;
  if (j < 0 || j >= ids.length) return;

  [ids[i], ids[j]] = [ids[j], ids[i]];

  // Single statement so the reorder is atomic — a partial rewrite would leave
  // the list in an order nobody chose.
  await sql`
    update todos as t
    set position = v.pos, updated_at = now()
    from (
      select id, ordinality::int as pos
      from unnest(${ids}::uuid[]) with ordinality as u(id, ordinality)
    ) as v
    where t.id = v.id
  `;
  revalidatePath("/");
}
