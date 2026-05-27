#!/usr/bin/env python3
"""
worker_launch.py — Download model and start dllama worker.

Usage:
  python3 worker_launch.py <model>               # auto-discover root server
  python3 worker_launch.py <model> --server IP   # use explicit root IP
  python3 worker_launch.py                       # list available models

Options:
  --server IP      Root server IP (skips discovery)
  --port PORT      Worker port (default: 9999)
  --root-port PORT Root discovery port to scan (default: 9990)
  --nthreads N     CPU threads (default: auto-detect)
  --cache-dir DIR  Cache dir for weights (default: ./models/<model>)
  -y               Skip confirmation prompts
"""

import os
import sys
import socket
import multiprocessing
import subprocess
import concurrent.futures
import importlib.util

# ---------------------------------------------------------------------------
# Load MODELS dict from launch.py without executing its __main__ block
# ---------------------------------------------------------------------------
def _load_models():
    spec = importlib.util.spec_from_file_location(
        "launch", os.path.join(os.path.dirname(__file__), "launch.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.MODELS, mod.downloadFile, mod.confirm

MODELS, _downloadFile, _confirm = _load_models()


# ---------------------------------------------------------------------------
# Network discovery
# ---------------------------------------------------------------------------
def _local_ips():
    ips = []
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.append(ip)
    except Exception:
        pass
    if not ips:
        # fallback: connect outward and read source address
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ips.append(s.getsockname()[0])
            s.close()
        except Exception:
            pass
    return list(set(ips))


def _probe_port(ip, port, timeout=0.3):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        result = s.connect_ex((ip, port))
        s.close()
        return result == 0
    except Exception:
        return False


def discover_root(root_port=9990):
    local_ips = _local_ips()
    if not local_ips:
        print("Could not detect local IP address.")
        return None

    subnets = set()
    for ip in local_ips:
        parts = ip.rsplit(".", 1)
        if len(parts) == 2:
            subnets.add(parts[0])

    candidates = []
    for subnet in subnets:
        for i in range(1, 255):
            candidate = f"{subnet}.{i}"
            if candidate not in local_ips:
                candidates.append(candidate)

    print(f"Scanning {len(candidates)} IPs on subnet(s) {', '.join(subnets)} for port {root_port}...")

    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=128) as pool:
        futures = {pool.submit(_probe_port, ip, root_port): ip for ip in candidates}
        for future in concurrent.futures.as_completed(futures):
            ip = futures[future]
            if future.result():
                print(f"  Found root server at {ip}:{root_port}")
                found.append(ip)

    if not found:
        return None
    if len(found) == 1:
        return found[0]

    print("Multiple root servers found:")
    for i, ip in enumerate(found):
        print(f"  [{i}] {ip}")
    idx = input("Select [0]: ").strip()
    try:
        return found[int(idx)] if idx else found[0]
    except (ValueError, IndexError):
        return found[0]


# ---------------------------------------------------------------------------
# Model download
# ---------------------------------------------------------------------------
def download_worker_model(model_name, model):
    dir_path = os.path.join("models", model_name)
    os.makedirs(dir_path, exist_ok=True)
    model_path = os.path.join(dir_path, f"dllama_model_{model_name}.m")
    tokenizer_path = os.path.join(dir_path, f"dllama_tokenizer_{model_name}.t")
    print(f"Downloading {model_name} to {dir_path}...")
    _downloadFile(model[0], model_path)
    _downloadFile([model[1]], tokenizer_path)
    return model_path, tokenizer_path


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def print_usage():
    print(__doc__)
    print("Available models:")
    for name in MODELS:
        print(f"  {name}")


def parse_arg(flag, default=None):
    try:
        idx = sys.argv.index(flag)
        return sys.argv[idx + 1]
    except (ValueError, IndexError):
        return default


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        print_usage()
        sys.exit(0 if "--help" in sys.argv or "-h" in sys.argv else 1)

    model_name = sys.argv[1].replace("-", "_")
    if model_name not in MODELS:
        print(f"Unknown model: {model_name}")
        print_usage()
        sys.exit(1)

    model = MODELS[model_name]

    worker_port = int(parse_arg("--port", "9999"))
    root_port   = int(parse_arg("--root-port", "9990"))
    nthreads    = int(parse_arg("--nthreads", str(multiprocessing.cpu_count())))
    explicit_server = parse_arg("--server")
    cache_dir   = parse_arg("--cache-dir", os.path.join("models", model_name))

    # 1. Download model
    model_path = os.path.join(cache_dir, f"dllama_model_{model_name}.m")
    tokenizer_path = os.path.join(cache_dir, f"dllama_tokenizer_{model_name}.t")

    if not os.path.isfile(model_path) or not os.path.isfile(tokenizer_path):
        if not _confirm(f"Download {model_name} to {cache_dir}?"):
            sys.exit(0)
        model_path, tokenizer_path = download_worker_model(model_name, model)
    else:
        print(f"Model already cached at {cache_dir}")

    # 2. Discover root server
    if explicit_server:
        root_ip = explicit_server
        print(f"Using explicit root server: {root_ip}")
    else:
        root_ip = discover_root(root_port)
        if not root_ip:
            print("Root server not found on local network.")
            root_ip = input("Enter root server IP manually (or leave blank to run without root): ").strip()
            if not root_ip:
                root_ip = None

    # 3. Build dllama command
    dllama = "./dllama.exe" if sys.platform == "win32" else "./dllama"
    cmd = [
        dllama, "worker",
        "--model", model_path,
        "--tokenizer", tokenizer_path,
        "--port", str(worker_port),
        "--nthreads", str(nthreads),
    ]
    if root_ip:
        cmd += ["--server", f"{root_ip}:{root_port}"]
    if cache_dir:
        cmd += ["--cache-dir", cache_dir]

    print(f"\nStarting worker: {' '.join(cmd)}\n")
    subprocess.run(cmd)


if __name__ == "__main__":
    main()
