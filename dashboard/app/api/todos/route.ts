import { NextResponse } from "next/server";
import { sql } from "@/lib/db";
import { checkAuth, reqString, badRequest, BadRequest } from "@/lib/auth";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// Spec §7
// GET                  -> [{id, title, position, done}], open items only,
//                         position-ordered. Called by the hub and dashboard.
// POST/PATCH/DELETE    -> CRUD + reorder, dashboard only.

// GET is unauthenticated on purpose: the hub fetches it on a timer, the list
// is not sensitive, and baking the token into a GET buys nothing. Every
// mutating verb below is authenticated.
export async function GET() {
  const rows = await sql`
    select id, title, position, done
    from todos
    where done = false
    order by position asc, updated_at asc
  `;
  return NextResponse.json(rows, {
    headers: { "cache-control": "no-store" },
  });
}

export async function POST(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const b = await req.json();
    const title = reqString(b.title, "title", 200);

    // Append to the end unless a position is given.
    const rows = await sql`
      insert into todos (title, position)
      values (
        ${title},
        ${typeof b.position === "number"
            ? b.position
            : sql`(select coalesce(max(position), 0) + 1 from todos)`}
      )
      returning id, title, position, done
    `;
    return NextResponse.json(rows[0], { status: 201 });
  } catch (e) {
    return badRequest(e);
  }
}

export async function PATCH(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const b = await req.json();

    // Reorder: { order: [id, id, id, ...] } — positions become array indices.
    if (Array.isArray(b.order)) {
      if (b.order.length > 200) throw new BadRequest("order is too long");
      const ids = b.order.map((id: unknown, i: number) => {
        if (typeof id !== "string") throw new BadRequest(`order[${i}] must be a uuid`);
        return id;
      });

      // Single statement so the reorder is atomic. A partial reorder would
      // leave the list in an order nobody chose.
      await sql`
        update todos as t
        set position = v.pos, updated_at = now()
        from (
          select id, ordinality::int as pos
          from unnest(${ids}::uuid[]) with ordinality as u(id, ordinality)
        ) as v
        where t.id = v.id
      `;
      return NextResponse.json({ ok: true, reordered: ids.length });
    }

    // Single-item edit: { id, title?, done? }
    const id = reqString(b.id, "id", 64);
    const title = b.title === undefined ? null : reqString(b.title, "title", 200);
    const done  = b.done  === undefined ? null : Boolean(b.done);

    if (title === null && done === null) {
      throw new BadRequest("nothing to update — send title, done, or order");
    }

    const rows = await sql`
      update todos
      set title      = coalesce(${title}, title),
          done       = coalesce(${done}, done),
          updated_at = now()
      where id = ${id}::uuid
      returning id, title, position, done
    `;
    if (rows.length === 0) {
      return NextResponse.json({ error: "not found" }, { status: 404 });
    }
    return NextResponse.json(rows[0]);
  } catch (e) {
    return badRequest(e);
  }
}

export async function DELETE(req: Request) {
  const denied = checkAuth(req);
  if (denied) return denied;

  try {
    const id = new URL(req.url).searchParams.get("id");
    if (!id) throw new BadRequest("id query parameter is required");

    const rows = await sql`delete from todos where id = ${id}::uuid returning id`;
    if (rows.length === 0) {
      return NextResponse.json({ error: "not found" }, { status: 404 });
    }
    return NextResponse.json({ ok: true });
  } catch (e) {
    return badRequest(e);
  }
}