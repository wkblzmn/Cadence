-- ─────────────────────────────────────────────────────────────────────────
--  Cadence — database schema
--  Target: Neon (PostgreSQL 16). Plain Postgres — no provider-specific SQL.
--
--  Part 1 is spec §6, unchanged.
--  Part 2 is the fold that turns an event stream into hours worked. §4 makes
--  this load-bearing: the device emits events and never totals, so every
--  number the dashboard shows has to be derived here.
--
--  Run in the Neon SQL editor, top to bottom.
-- ─────────────────────────────────────────────────────────────────────────

-- ═══════════════════════════════════════════ part 1 — tables (spec §6)

create table if not exists devices (
  id           text primary key,          -- 'cadence-hub-01'
  name         text not null,
  created_at   timestamptz default now()
);

create table if not exists readings (
  id           bigserial primary key,
  device_id    text references devices(id),
  ts           timestamptz not null,
  temp_c       real,
  humidity     real,
  pressure_hpa real,
  lux          real,
  real_feel_c  real
);
create index if not exists readings_device_ts_idx on readings (device_id, ts desc);

create table if not exists session_events (
  id           bigserial primary key,
  device_id    text references devices(id),
  ts           timestamptz not null,
  type         text not null check (type in ('start','pause','resume','stop')),
  source       text not null check (source in ('button','vision'))
);
create index if not exists session_events_device_ts_idx on session_events (device_id, ts);

create table if not exists focus_samples (
  id           bigserial primary key,
  device_id    text references devices(id),
  ts           timestamptz not null,
  state        text not null check (state in ('focused','distracted','absent')),
  confidence   real
);
create index if not exists focus_samples_device_ts_idx on focus_samples (device_id, ts);

create table if not exists todos (
  id           uuid primary key default gen_random_uuid(),
  title        text not null,
  position     int  not null,
  done         boolean not null default false,
  updated_at   timestamptz default now()
);

insert into devices (id, name)
values ('cadence-hub-01', 'Cadence hub')
on conflict (id) do nothing;

-- Idempotency guards. The hub retries a failed POST, and without these a
-- flaky link produces duplicate rows, which inflate every total downstream.
create unique index if not exists session_events_dedupe_idx
  on session_events (device_id, ts, type);

create unique index if not exists readings_dedupe_idx
  on readings (device_id, ts);


-- ═══════════════════════════════════════════ part 2 — the fold

-- ── 2a. Pair each opening event with whatever closes it ──────────────────
--
-- 'start' and 'resume' open an interval. 'pause' and 'stop' close one.
-- The next event in time order closes the current one, whatever it is — a
-- second 'start' with no intervening 'stop' still terminates the interval,
-- because two overlapping sessions are not a thing this device can produce.
--
-- Three real-world cases this has to survive:
--   1. Power loss mid-session. The opening event has no closing event. The
--      interval is left open and clamped to now(); see 2b.
--   2. Duplicate 'pause' with no preceding open. Produces no interval.
--   3. A session running across midnight. Handled in 2c by splitting on the
--      day boundary rather than attributing the whole block to one day.

create or replace view session_intervals as
with ordered as (
  select
    device_id,
    ts,
    type,
    source,
    lead(ts)   over (partition by device_id order by ts, id) as next_ts,
    lead(type) over (partition by device_id order by ts, id) as next_type
  from session_events
)
select
  device_id,
  source,
  ts                                        as started_at,
  coalesce(next_ts, now())                  as ended_at,
  next_type is null                         as still_open,
  coalesce(next_ts, now()) - ts             as duration
from ordered
where type in ('start', 'resume')
  -- Guard against a clock skew or duplicate producing a negative interval.
  and coalesce(next_ts, now()) > ts;


-- ── 2b. Sanity-clamped intervals ─────────────────────────────────────────
--
-- An interval left open by a power loss would otherwise grow forever and
-- silently corrupt every total. Cap open intervals at 4 hours: longer than
-- any plausible unbroken focus session, short enough that a crash costs one
-- afternoon rather than a week.

create or replace view session_intervals_clamped as
select
  device_id,
  source,
  started_at,
  case
    when still_open and ended_at - started_at > interval '4 hours'
      then started_at + interval '4 hours'
    else ended_at
  end as ended_at,
  still_open
from session_intervals;


-- ── 2c. Split intervals at local-day boundaries ──────────────────────────
--
-- A session from 23:30 to 00:45 is 30 minutes on one day and 45 on the next.
-- Attributing all 75 minutes to the start date makes the daily chart lie.
--
-- Days are local (Asia/Dhaka), not UTC — a "day" is what the user experienced.
-- Storage stays UTC; only the bucketing is local.

create or replace function session_daily(
  p_device_id text,
  p_from      date,
  p_to        date,
  p_tz        text default 'Asia/Dhaka'
)
returns table (
  day           date,
  focus_seconds bigint,
  session_count bigint
)
language sql
stable
as $$
  with bounds as (
    select
      i.started_at,
      i.ended_at,
      generate_series(
        date_trunc('day', i.started_at at time zone p_tz),
        date_trunc('day', i.ended_at   at time zone p_tz),
        interval '1 day'
      ) as local_day_start
    from session_intervals_clamped i
    where i.device_id = p_device_id
  ),
  sliced as (
    select
      local_day_start::date as day,
      greatest(started_at, (local_day_start                  at time zone p_tz)) as seg_start,
      least   (ended_at,   (local_day_start + interval '1 day') at time zone p_tz) as seg_end
    from bounds
  )
  select
    day,
    sum(extract(epoch from (seg_end - seg_start)))::bigint as focus_seconds,
    count(*)::bigint                                       as session_count
  from sliced
  where seg_end > seg_start
    and day between p_from and p_to
  group by day
  order by day;
$$;


-- ── 2d. Convenience views for the dashboard ──────────────────────────────

create or replace view session_daily_totals as
select
  device_id,
  (started_at at time zone 'Asia/Dhaka')::date as day,
  sum(extract(epoch from (ended_at - started_at)))::bigint as focus_seconds,
  count(*) as segments
from session_intervals_clamped
group by device_id, day
order by day desc;

-- Longest unbroken stretch per day. §8 calls for sustained focus; this is the
-- standalone-mode version, before the vision tier adds attention quality.
create or replace view session_longest_stretch as
select
  device_id,
  (started_at at time zone 'Asia/Dhaka')::date as day,
  max(extract(epoch from (ended_at - started_at)))::bigint as longest_seconds
from session_intervals_clamped
group by device_id, day;

create or replace view readings_latest as
select distinct on (device_id) *
from readings
order by device_id, ts desc;


-- ═══════════════════════════════════════════ part 3 — access control
--
-- Host is Neon, not Supabase (spec §5 amended — the load-bearing requirement
-- was PostgreSQL, not a specific provider). Neon has no anon key and no
-- browser-direct access, so there is no RLS to configure: the connection
-- string never leaves the server.
--
-- Access model:
--   hub       -> POST with the static bearer token, checked in the API route
--   dashboard -> Next.js server components and route handlers only
--   browser   -> never talks to Postgres directly
--
-- Keep DATABASE_URL in .env.local and out of git. If it ever leaks, rotate it
-- from the Neon console — it is the only thing standing in front of the data.


-- ═══════════════════════════════════════════ smoke test
--
-- Uncomment, run, and check the numbers before wiring the API. Expect
-- 25 min on day one (20 + 5) and 45 min spanning midnight into day two.
--
-- insert into session_events (device_id, ts, type, source) values
--   ('cadence-hub-01', '2026-08-11 09:00:00+06', 'start',  'button'),
--   ('cadence-hub-01', '2026-08-11 09:20:00+06', 'pause',  'button'),
--   ('cadence-hub-01', '2026-08-11 09:25:00+06', 'resume', 'button'),
--   ('cadence-hub-01', '2026-08-11 09:30:00+06', 'stop',   'button'),
--   ('cadence-hub-01', '2026-08-11 23:30:00+06', 'start',  'button'),
--   ('cadence-hub-01', '2026-08-12 00:15:00+06', 'stop',   'button');
--
-- select * from session_daily('cadence-hub-01', '2026-08-01', '2026-08-31');
-- Expect: 2026-08-11 -> 3600 s (25 min + 30 min), 2026-08-12 -> 900 s.