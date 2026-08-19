"""Entry point.  `python -m cadence`  or  `pythonw cadence.pyw`."""

import argparse
import sys

from . import config as cfg


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="cadence",
        description="Cadence — the attention pipeline, on the PC.")
    ap.add_argument("--host", default=cfg.DEFAULT_HOST,
                    help=f"camera board hostname or IP (default: {cfg.DEFAULT_HOST})")
    ap.add_argument("--local", action="store_true",
                    help="start in local mode: this app owns the session, "
                         "not the hub's focus-timer button")
    ap.add_argument("--calibrate", action="store_true",
                    help="measure the baseline on the console and exit, "
                         "without opening the window")
    ap.add_argument("--no-post", action="store_true",
                    help="classify and display, but write nothing to the backend")
    ap.add_argument("--source", choices=("usb", "wifi"), default=cfg.DEFAULT_SOURCE,
                    help="where frames come from (default: %s). usb needs no "
                         "network of any kind" % cfg.DEFAULT_SOURCE)
    ap.add_argument("--port", default=None,
                    help="serial port for --source usb, e.g. COM4. "
                         "Auto-detected and remembered if omitted")
    ap.add_argument("--hub", action="store_true",
                    help="follow the hub's focus-timer button even on USB "
                         "(needs the hub and camera reachable over the network)")
    ap.add_argument("--no-tune", action="store_true",
                    help="leave the board's camera settings exactly as they are")
    a = ap.parse_args(argv)

    if a.calibrate:
        from .pipeline import calibrate
        try:
            calibrate(a.host, source=a.source)
        except Exception as e:
            sys.exit(str(e))
        return

    cfg.log(f"Cadence starting — {a.source}, "
            f"{'local' if a.local else 'hub'} mode, "
            f"posting {'off' if a.no_post else 'on'}")

    # Over the cable there is no network to ask the hub about, so the session
    # has to be this app's own. The radio buttons still switch it at runtime for
    # anyone running USB frames on a desk where the hub *is* reachable.
    mode = "local" if (a.local or (a.source == "usb" and not a.hub)) else "hub"
    if a.source == "usb" and mode == "local" and not a.local:
        cfg.log("USB source — session control is the app's Start button "
                "(pass --hub to follow the timer button instead)")

    from .ui import run
    run(a.host, post=not a.no_post, mode=mode,
        tune=not a.no_tune, source=a.source, port=a.port)


if __name__ == "__main__":
    main()
