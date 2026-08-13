import { createTodo, setDone, renameTodo, deleteTodo, moveTodo } from "./actions";

export type Todo = {
  id: string;
  title: string;
  position: number;
  done: boolean;
};

// Every control is its own <form> bound to a server action. Forms cannot nest,
// so a row is a flex strip of sibling forms rather than one form with several
// submit buttons. The upside is that the whole view works with no client
// JavaScript at all — which is the right trade for a device dashboard.

const btn =
  "inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-md border text-xs transition-colors";

function IconButton({
  label,
  glyph,
  disabled = false,
  danger = false,
}: {
  label: string;
  glyph: string;
  disabled?: boolean;
  danger?: boolean;
}) {
  return (
    <button
      type="submit"
      aria-label={label}
      title={label}
      disabled={disabled}
      className={`${btn} ${disabled ? "cursor-default opacity-30" : "hover:bg-black/[.04] dark:hover:bg-white/[.06]"}`}
      style={{
        borderColor: "var(--hairline)",
        color: danger ? "var(--status-critical)" : "var(--text-secondary)",
      }}
    >
      <span aria-hidden>{glyph}</span>
    </button>
  );
}

function Row({
  todo,
  first,
  last,
}: {
  todo: Todo;
  first: boolean;
  last: boolean;
}) {
  return (
    <li
      className="flex items-center gap-2 border-b py-2 last:border-b-0"
      style={{ borderColor: "var(--hairline)" }}
    >
      <form action={setDone}>
        <input type="hidden" name="id" value={todo.id} />
        <input type="hidden" name="done" value={todo.done ? "false" : "true"} />
        <IconButton
          label={todo.done ? "Mark as not done" : "Mark as done"}
          glyph={todo.done ? "✓" : "○"}
        />
      </form>

      {/* The Save button is load-bearing, not decoration: without a submit
          button this form does not submit on Enter, so an Enter-only rename
          silently did nothing. It is also plainly visible rather than
          revealed on focus — a control you cannot see is a control a mouse
          user cannot find, and hiding it until focus made the edit
          undiscoverable and the target un-clickable. */}
      <form action={renameTodo} className="flex min-w-0 flex-1 items-center gap-1">
        <input type="hidden" name="id" value={todo.id} />
        <input
          name="title"
          defaultValue={todo.title}
          maxLength={200}
          aria-label="Task title"
          className="w-full truncate rounded-md border border-transparent bg-transparent px-2 py-1 text-sm outline-none focus:border-[color:var(--hairline)]"
          style={{
            color: todo.done ? "var(--text-muted)" : "var(--text-primary)",
            textDecoration: todo.done ? "line-through" : undefined,
          }}
        />
        <button
          type="submit"
          aria-label="Save title"
          title="Save title"
          className={`${btn} hover:bg-black/[.04] dark:hover:bg-white/[.06]`}
          style={{
            borderColor: "var(--hairline)",
            color: "var(--text-secondary)",
          }}
        >
          <span aria-hidden>↵</span>
        </button>
      </form>

      {!todo.done && (
        <>
          <form action={moveTodo}>
            <input type="hidden" name="id" value={todo.id} />
            <input type="hidden" name="dir" value="up" />
            <IconButton label="Move up" glyph="↑" disabled={first} />
          </form>
          <form action={moveTodo}>
            <input type="hidden" name="id" value={todo.id} />
            <input type="hidden" name="dir" value="down" />
            <IconButton label="Move down" glyph="↓" disabled={last} />
          </form>
        </>
      )}

      <form action={deleteTodo}>
        <input type="hidden" name="id" value={todo.id} />
        <IconButton label="Delete task" glyph="×" danger />
      </form>
    </li>
  );
}

export default function Tasks({ todos }: { todos: Todo[] }) {
  const open = todos.filter((t) => !t.done);
  const done = todos.filter((t) => t.done);

  return (
    <div>
      <form action={createTodo} className="mb-3 flex gap-2">
        <input
          name="title"
          placeholder="Add a task"
          maxLength={200}
          required
          aria-label="New task title"
          className="min-w-0 flex-1 rounded-md border px-3 py-1.5 text-sm outline-none focus:border-[color:var(--series-1)]"
          style={{
            borderColor: "var(--hairline)",
            color: "var(--text-primary)",
            background: "transparent",
          }}
        />
        <button
          type="submit"
          className="rounded-md px-3 py-1.5 text-sm font-medium text-white transition-opacity hover:opacity-90"
          style={{ background: "var(--series-1)" }}
        >
          Add
        </button>
      </form>

      {open.length === 0 ? (
        <p className="py-2 text-sm" style={{ color: "var(--text-muted)" }}>
          Nothing open.
        </p>
      ) : (
        <ul>
          {open.map((t, i) => (
            <Row
              key={t.id}
              todo={t}
              first={i === 0}
              last={i === open.length - 1}
            />
          ))}
        </ul>
      )}

      {done.length > 0 && (
        <details className="mt-3">
          <summary
            className="cursor-pointer text-xs"
            style={{ color: "var(--text-muted)" }}
          >
            {done.length} completed
          </summary>
          <ul className="mt-1">
            {done.map((t) => (
              <Row key={t.id} todo={t} first last />
            ))}
          </ul>
        </details>
      )}

      <p className="mt-3 text-xs" style={{ color: "var(--text-muted)" }}>
        The hub reads open tasks from <code>GET /api/todos</code>, position-ordered.
      </p>
    </div>
  );
}
