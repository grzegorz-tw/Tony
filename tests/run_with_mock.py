#!/usr/bin/env python3
"""Starts tests/mock_llama.py on a free port, runs the given test executable against it, then stops the mock."""
import os, socket, subprocess, sys, time

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

port = free_port()
mock = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_llama.py"), str(port)])
try:
    for _ in range(50):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)
    sys.exit(subprocess.call([sys.argv[1], str(port)]))
finally:
    mock.terminate()
