# Double-click launcher: pythonw runs this with no console window.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cadence.__main__ import main
main()
