#!/usr/bin/env python3
import sys
import os

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} flash.bin size_bytes")
        raise SystemExit(1)
    path = sys.argv[1]
    size = int(sys.argv[2])
    with open(path, "wb") as f:
        f.truncate(size)
    print(f"Created {path} with size {size} bytes")

if __name__ == "__main__":
    main()
