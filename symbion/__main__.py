"""Entry point for `python -m symbion`."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from symbion_v13 import main

if __name__ == "__main__":
    main()
