"use client";

import { useEffect, useState } from "react";

// Three states, not two. The palette in globals.css is already built for it:
// no data-theme attribute means "follow the OS", and an explicit stamp wins in
// either direction. A two-way toggle would strand anyone who flipped it once
// with no way back to following their system.

type Mode = "system" | "light" | "dark";

const NEXT: Record<Mode, Mode> = { system: "light", light: "dark", dark: "system" };
const LABEL: Record<Mode, string> = { system: "System", light: "Light", dark: "Dark" };
const GLYPH: Record<Mode, string> = { system: "◐", light: "☀", dark: "☾" };

const KEY = "cadence-theme";

export default function ThemeToggle() {
  const [mode, setMode] = useState<Mode>("system");

  // The inline script in layout.tsx has already applied the stored choice
  // before first paint. This only syncs React's copy after hydration, so the
  // server and client agree on the first render and there is no flash.
  useEffect(() => {
    try {
      const stored = localStorage.getItem(KEY);
      if (stored === "light" || stored === "dark") setMode(stored);
    } catch {
      // Private mode or blocked storage — stay on "system".
    }
  }, []);

  function apply(next: Mode) {
    setMode(next);
    const root = document.documentElement;
    try {
      if (next === "system") {
        delete root.dataset.theme;
        localStorage.removeItem(KEY);
      } else {
        root.dataset.theme = next;
        localStorage.setItem(KEY, next);
      }
    } catch {
      // Storage can fail; the attribute still applies for this session.
    }
  }

  return (
    <button
      type="button"
      onClick={() => apply(NEXT[mode])}
      aria-label={`Theme: ${LABEL[mode]}. Activate to switch.`}
      title={`Theme: ${LABEL[mode]}`}
      className="inline-flex shrink-0 items-center gap-2 rounded-full border px-3 py-1.5 text-xs font-medium transition-colors hover:bg-black/[.04] dark:hover:bg-white/[.06]"
      style={{ borderColor: "var(--hairline)", color: "var(--text-secondary)" }}
    >
      <span aria-hidden>{GLYPH[mode]}</span>
      {LABEL[mode]}
    </button>
  );
}
