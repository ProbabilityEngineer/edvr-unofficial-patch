#!/usr/bin/env python3
"""Explicit requests for the temporary flat draw-isolation diagnostic."""
import argparse
import json
import os
from pathlib import Path
import re
import tempfile
import time
import uuid


def token(value):
    if not re.fullmatch(r"[0-9a-f]{32}", value):
        raise ValueError("expected a 32-character lowercase experiment ID")
    return value


def publish(root, command, dataset="-", first=0, last=0, dry_run=False):
    root = Path(root)
    if not root.is_dir():
        raise ValueError("--log-dir must be an existing EDVR log directory")
    if command not in ("discover", "test", "cancel"):
        raise ValueError("unknown command")
    if command == "test":
        token(dataset)
        if not 1 <= first <= last <= 8192:
            raise ValueError("group range must be within 1..8192")
    elif (dataset, first, last) != ("-", 0, 0):
        raise ValueError("only test accepts a dataset and range")
    request = uuid.uuid4().hex
    payload = f"EDVR_ISOLATE_V1 {int(time.time())} {request} {command} {dataset} {first} {last}\n"
    if not dry_run:
        temporary = root / f".flat_draw_isolation.{request}.tmp"
        created = False
        try:
            stream = temporary.open("x", encoding="ascii")
            created = True
            with stream:
                stream.write(payload)
            # Publish complete bytes atomically WITHOUT replacing another
            # operator's pending request. Supported on APFS and NTFS.
            os.link(temporary, root / "flat_draw_isolation.request")
        finally:
            if created:
                temporary.unlink(missing_ok=True)
    return request, payload


def report(root, request, preview=None, dry_run=False):
    directory = Path(root) / "flat_draw_isolation" / token(request)
    path = directory / "manifest.json"
    if path.stat().st_size > 8 * 1024 * 1024:
        raise ValueError("oversized manifest")
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "edvr-flat-draw-isolation-v1" or data.get("request") != request:
        raise ValueError("manifest identity/schema mismatch")
    groups = data["groups"]
    if len(groups) > 8192 or [g["id"] for g in groups] != list(range(1, len(groups) + 1)):
        raise ValueError("invalid group IDs")
    for g in groups:
        if len(g["key"]) != 18 or any(not re.fullmatch(r"[0-9a-f]{1,16}", x) for x in g["key"]):
            raise ValueError("invalid signature")
        if not 0 <= g["masked"] <= g["selected"] <= g["seen"]:
            raise ValueError("invalid issued-call accounting")
    if data["masked_issues"] != sum(g["masked"] for g in groups):
        raise ValueError("masked count mismatch")
    pictures = []
    if len(data["images"]) != 3:
        raise ValueError("expected normal/suppressed/restored references")
    for i, im in enumerate(data["images"]):
        if im["label"] != ("normal", "suppressed", "restored")[i] or im["file"] != f"image_{i}.bin":
            raise ValueError("invalid image identity")
        if im["status"] != "ok":
            continue
        w, h, fmt = im["width"], im["height"], im["format"]
        if not 1 <= w <= 512 or not 1 <= h <= 512 or fmt not in (28, 29, 87, 91):
            raise ValueError("invalid image extent/format")
        payload = directory / im["file"]
        if im["bytes"] != w * h * 4 or payload.stat().st_size != im["bytes"]:
            raise ValueError("invalid image byte count")
        pictures.append((im, payload.read_bytes()))
    if data["test_complete"] and (not data["masked_issues"] or len(pictures) != 3 or data["reason"] != "test-ended"):
        raise ValueError("false completion claim")
    if preview is not None:
        preview = Path(preview).resolve()
        source = directory.resolve()
        if preview == source or source in preview.parents:
            raise ValueError("preview must be outside the capture directory")
        if preview.exists():
            raise ValueError("preview directory must be new")
        if not dry_run:
            preview.mkdir(parents=True, exist_ok=False)
            # Use the repository's existing bounded PNG encoder.
            from flat_pixels import write_png
            from flat_draw_pixels import reference_rows
            for im, rgba in pictures:
                write_png(preview / (im["label"] + ".png"), im["width"], im["height"], reference_rows(im, rgba))
    return data


def describe_group(group):
    w = [int(x, 16) for x in group["key"]]
    base = w[3] >> 32
    return {
        **{k: v for k, v in group.items() if k != "key"},
        "vs": f"{w[0]:016X}", "ps": f"{w[1]:016X}", "kind": chr(w[2] & 255),
        "count": (w[2] >> 8) & 0xffffffff, "topology": w[2] >> 40,
        "start": w[3] & 0xffffffff, "base": base if base < 0x80000000 else base - 0x100000000,
        "instances": w[4] & 0xffffffff, "start_instance": w[4] >> 32,
        "binding_lifetime_ids": {"vb0": w[5], "ib": w[6], "rtv0": w[7], "dsv": w[8],
                                 "layout": w[11], "ps_srvs_0_3": w[12:16], "vb1": w[16]},
    }


def self_test():
    import contextlib
    import io
    from unittest.mock import patch
    key = ["0"] * 18
    key[2] = f"{ord('X') | (432 << 8) | (4 << 40):x}"
    key[3] = f"{(0xffffffff << 32) | 4:x}"
    key[4] = f"{(5 << 32) | 3:x}"
    decoded = describe_group({"key": key, "id": 1})
    assert (decoded["kind"], decoded["count"], decoded["base"], decoded["instances"], decoded["start_instance"]) == ("X", 432, -1, 3, 5)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        before = list(root.iterdir())
        _, text = publish(root, "discover", dry_run=True)
        assert list(root.iterdir()) == before and len(text.split()) == 7
        request, text = publish(root, "discover")
        pending = root / "flat_draw_isolation.request"
        assert pending.read_text() == text
        try:
            publish(root, "cancel")
            raise AssertionError("replaced pending request")
        except FileExistsError:
            pass
        assert pending.read_text() == text and len(list(root.iterdir())) == 1
        pending.unlink()
        collision = root / f".flat_draw_isolation.{request}.tmp"
        collision.write_text("another publisher")
        with patch.object(uuid, "uuid4", return_value=uuid.UUID(request)):
            try:
                publish(root, "cancel")
                raise AssertionError("reused another publisher's temporary")
            except FileExistsError:
                pass
        assert collision.exists() and collision.read_text() == "another publisher"
        collision.unlink()
        publish(root, "test", request, 1, 8192, dry_run=True)
        for bad in ("../escape", "a" * 33, "A" * 32):
            try:
                token(bad)
                raise AssertionError("accepted invalid ID")
            except ValueError:
                pass
        for first, last in ((0, 1), (2, 1), (1, 8193)):
            try:
                publish(root, "test", request, first, last, dry_run=True)
                raise AssertionError("accepted invalid range")
            except ValueError:
                pass
        assert not list(root.iterdir())
        for command in ("discover", "test", "cancel"):
            arguments = [command, "--log-dir", tmp, "--dry-run"]
            if command == "test":
                arguments += ["--dataset", request, "--first", "1", "--last", "2"]
            with contextlib.redirect_stdout(io.StringIO()):
                main(arguments)
            assert not list(root.iterdir())
        directory = root / "flat_draw_isolation" / request
        directory.mkdir(parents=True)
        manifest = {
            "schema": "edvr-flat-draw-isolation-v1", "request": request, "command": "discover",
            "reason": "discovered", "test_complete": False, "masked_issues": 0, "groups": [],
            "images": [{"label": label, "status": "not-requested", "frame": 0, "file": f"image_{i}.bin"}
                       for i, label in enumerate(("normal", "suppressed", "restored"))],
        }
        path = directory / "manifest.json"
        path.write_text(json.dumps(manifest))
        before = list(root.rglob("*"))
        with contextlib.redirect_stdout(io.StringIO()):
            main(["report", "--log-dir", tmp, "--id", request, "--dry-run", "--preview-dir", str(root / "preview")])
        assert list(root.rglob("*")) == before
        manifest["test_complete"] = True
        path.write_text(json.dumps(manifest))
        try:
            report(root, request)
            raise AssertionError("accepted false completion")
        except ValueError:
            pass
    print("flat_draw_isolation self-test: passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("discover", "test", "cancel", "report"))
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--dataset", default="-")
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--last", type=int, default=0)
    parser.add_argument("--id")
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--groups", nargs=2, type=int, metavar=("FIRST", "LAST"))
    parser.add_argument("--wait", type=float, default=6, help="report wait, 0..10 seconds")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return
    if not args.command or not args.log_dir or not 0 <= args.wait <= 10:
        parser.error("command, --log-dir and --wait within 0..10 required")
    if args.groups and not 1 <= args.groups[0] <= args.groups[1] <= 8192:
        parser.error("--groups must be within 1..8192")
    if args.command == "report":
        if not args.id:
            parser.error("report requires --id")
        data = report(args.log_dir, args.id, args.preview_dir, args.dry_run)
    else:
        if args.preview_dir:
            parser.error("--preview-dir is only for report")
        request, payload = publish(args.log_dir, args.command, args.dataset, args.first, args.last, args.dry_run)
        print(("DRY RUN: " if args.dry_run else "SUBMITTED: ") + payload.strip(), flush=True)
        if args.dry_run or args.command == "cancel" or not args.wait:
            return
        end = time.monotonic() + args.wait
        while True:
            try:
                data = report(args.log_dir, request)
                break
            except FileNotFoundError:
                if time.monotonic() >= end:
                    raise ValueError("no completed manifest; inspect game log for acceptance/refusal, do not assume it ran")
                time.sleep(.1)
    summary = {k: v for k, v in data.items() if k not in ("groups", "images")}
    summary["group_count"] = len(data["groups"])
    summary["images"] = [{"label": i["label"], "status": i["status"], "frame": i["frame"]} for i in data["images"]]
    print(json.dumps(summary, indent=2))
    if args.groups:
        first, last = args.groups
        for g in data["groups"][max(0, first - 1):last]:
            print(json.dumps(describe_group(g)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f"flat_draw_isolation: {error}")
