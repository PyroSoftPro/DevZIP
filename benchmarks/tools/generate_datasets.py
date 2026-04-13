"""Generate standardized benchmark datasets for each file type category."""
from __future__ import annotations

import json
import os
import random
import shutil

BASE = os.path.join(os.path.dirname(__file__), "..", "..", "sample-data", "benchmarks")


def generate_text_corpus():
    text_dir = os.path.join(BASE, "text-corpus")
    os.makedirs(text_dir, exist_ok=True)

    src_dir = os.path.join(os.path.dirname(__file__), "..", "..", "native", "engine", "src")
    code_dir = os.path.join(text_dir, "source-code")
    if os.path.exists(code_dir):
        shutil.rmtree(code_dir)
    shutil.copytree(src_dir, code_dir)

    paras = [
        "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.",
        "How vexingly quick daft zebras jump! The five boxing wizards jump quickly.",
        "Sphinx of black quartz, judge my vow. Two driven jocks help fax my big quiz.",
        "Jackdaws love my big sphinx of quartz. Mr Jock, TV quiz PhD, bags few lynx.",
        "Crazy Frederick bought many very exquisite opal jewels.",
        "A mad boxer shot a quick, gloved jab to the jaw of his dizzy opponent.",
        "The job requires extra pluck and zeal from every young wage earner.",
        "Sixty zippers were quickly picked from the woven jute bag.",
    ]
    rng = random.Random(42)
    with open(os.path.join(text_dir, "prose-large.txt"), "w") as f:
        for _ in range(40000):
            f.write(rng.choice(paras) + "\n")

    rng = random.Random(99)
    names = ["Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Hank", "Iris", "Jack"]
    depts = ["Engineering", "Sales", "Marketing", "Support", "Finance", "Legal", "HR", "Ops"]
    with open(os.path.join(text_dir, "data-table.csv"), "w") as f:
        f.write("id,timestamp,name,email,department,value,score,notes\n")
        for i in range(30000):
            ts = f"2025-{rng.randint(1,12):02d}-{rng.randint(1,28):02d}T{rng.randint(0,23):02d}:{rng.randint(0,59):02d}:{rng.randint(0,59):02d}Z"
            note = f"note {i}" if rng.random() < 0.3 else ""
            f.write(
                f"{i},{ts},{rng.choice(names)},{rng.choice(names).lower()}@example.com,"
                f"{rng.choice(depts)},{rng.uniform(0,10000):.2f},{rng.randint(0,100)},{note}\n"
            )

    rng = random.Random(77)
    records = []
    for i in range(5000):
        records.append({
            "id": i,
            "name": f"item-{i:05d}",
            "tags": [f"tag-{rng.randint(0,50)}" for _ in range(rng.randint(1, 5))],
            "value": round(rng.uniform(0, 1000), 3),
            "active": rng.random() > 0.3,
        })
    with open(os.path.join(text_dir, "records.json"), "w") as f:
        json.dump(records, f, indent=2)

    rng = random.Random(55)
    with open(os.path.join(text_dir, "catalog.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<catalog>\n')
        for i in range(8000):
            f.write(f'  <item id="{i}" category="cat-{rng.randint(0,20)}">\n')
            f.write(f"    <name>Product {i:05d}</name>\n")
            f.write(f"    <price>{rng.uniform(1,500):.2f}</price>\n")
            f.write(f"    <description>A detailed description for product number {i}.</description>\n")
            f.write("  </item>\n")
        f.write("</catalog>\n")

    rng = random.Random(33)
    levels = ["INFO", "DEBUG", "WARN", "ERROR", "TRACE"]
    components = ["auth", "api", "db", "cache", "scheduler", "worker", "gateway"]
    with open(os.path.join(text_dir, "application.log"), "w") as f:
        for _ in range(15000):
            ts = (
                f"2025-03-{rng.randint(1,28):02d}T{rng.randint(0,23):02d}:"
                f"{rng.randint(0,59):02d}:{rng.randint(0,59):02d}.{rng.randint(0,999):03d}Z"
            )
            action = rng.choice(["completed", "failed", "started", "retried", "queued"])
            f.write(
                f"[{ts}] [{rng.choice(levels):5s}] [{rng.choice(components):10s}] "
                f"Request {action} id={rng.randint(10000,99999)}\n"
            )

    print("  text-corpus generated")


def generate_random_entropy():
    rand_dir = os.path.join(BASE, "random-entropy")
    os.makedirs(rand_dir, exist_ok=True)
    rng = random.Random(12345)
    for i in range(5):
        path = os.path.join(rand_dir, f"random-{i}.bin")
        if os.path.exists(path):
            continue
        data = bytes(rng.getrandbits(8) for _ in range(2 * 1024 * 1024))
        with open(path, "wb") as f:
            f.write(data)
    print("  random-entropy generated")


def assemble_software_binaries():
    sw_dir = os.path.join(BASE, "software-bin")
    os.makedirs(sw_dir, exist_ok=True)
    src_bin = os.path.join(
        os.path.dirname(__file__), "..", "..", "apps", "windows-ui",
        "DevZip.App.Tests", "bin", "Debug", "net8.0-windows",
    )
    for root, _dirs, files in os.walk(src_bin):
        for fn in files:
            if fn.endswith((".dll", ".exe")):
                src_path = os.path.join(root, fn)
                rel = os.path.relpath(root, src_bin)
                dst_path = os.path.join(sw_dir, rel, fn)
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                if not os.path.exists(dst_path):
                    shutil.copy2(src_path, dst_path)
    print("  software-bin assembled")


def generate_video_clips():
    vid_dir = os.path.join(BASE, "video-clips")
    os.makedirs(vid_dir, exist_ok=True)
    path = os.path.join(vid_dir, "synthetic-raw.yuv")
    if os.path.exists(path):
        print("  video-clips already exists")
        return

    width, height, fps, seconds = 320, 240, 15, 5
    frame_size = width * height * 3
    with open(path, "wb") as f:
        for frame in range(fps * seconds):
            data = bytearray(frame_size)
            for y in range(height):
                for x in range(width):
                    idx = (y * width + x) * 3
                    data[idx] = (x * 255 // width + frame * 3) & 0xFF
                    data[idx + 1] = (y * 255 // height + frame * 2) & 0xFF
                    data[idx + 2] = ((x + y) * 128 // (width + height) + frame) & 0xFF
            f.write(bytes(data))
    print("  video-clips generated")


def print_summary():
    for name in ["text-corpus", "random-entropy", "software-bin", "video-clips"]:
        d = os.path.join(BASE, name)
        if not os.path.exists(d):
            continue
        total = sum(
            os.path.getsize(os.path.join(r, fn))
            for r, _, files in os.walk(d)
            for fn in files
        )
        count = sum(len(files) for _, _, files in os.walk(d))
        print(f"  {name}: {count} files, {total / 1024 / 1024:.2f} MB")


if __name__ == "__main__":
    print("Generating benchmark datasets...")
    generate_text_corpus()
    generate_random_entropy()
    assemble_software_binaries()
    generate_video_clips()
    print("\nDataset summary:")
    print_summary()
