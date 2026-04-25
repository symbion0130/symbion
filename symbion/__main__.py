"""Entry point for `python -m symbion` and the `symbion` console script."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from symbion_v14 import main as _main

def main():
    _main()

if __name__ == "__main__":
    main()
