"""Host system metrics sampler — CPU, RAM, GPU/VRAM, network throughput."""

import shutil
import subprocess
import time

import psutil

try:
    import GPUtil
    _HAS_GPUTIL = True
except ImportError:
    _HAS_GPUTIL = False


class SystemMonitor:
    """Samples host resource usage on each call to :meth:`get_stats`.

    Network throughput is computed as the delta since the previous call,
    so the first sample always returns 0 KB/s for network rates.

    GPU sampling uses GPUtil when available. The nvidia-smi fallback result
    is cached for GPU_CACHE_TTL seconds to avoid spawning a subprocess every
    second.
    """

    GPU_CACHE_TTL = 5.0

    def __init__(self):
        self._prev_net      = psutil.net_io_counters()
        self._prev_time     = time.time()
        self._gpu_cache     = None   # cached (gpu, vram) from nvidia-smi
        self._gpu_cache_exp = 0.0    # expiry timestamp

    def get_stats(self):
        """Return ``(cpu, ram, gpu, vram, net_up_kbps, net_down_kbps)`` as ints."""
        cpu = int(psutil.cpu_percent())
        ram = int(psutil.virtual_memory().percent)
        gpu, vram = self._sample_gpu()

        now_time = time.time()
        now_net  = psutil.net_io_counters()
        dt = max(now_time - self._prev_time, 1e-9)

        net_up   = int(((now_net.bytes_sent - self._prev_net.bytes_sent) / 1024) / dt)
        net_down = int(((now_net.bytes_recv - self._prev_net.bytes_recv) / 1024) / dt)

        self._prev_net  = now_net
        self._prev_time = now_time

        return cpu, ram, gpu, vram, net_up, net_down

    # ------------------------------------------------------------------
    # GPU sampling — tries GPUtil first, then nvidia-smi, then returns 0.
    # ------------------------------------------------------------------

    def _sample_gpu(self):
        if _HAS_GPUTIL:
            result = self._gpu_via_gputil()
            if result is not None:
                return result

        # nvidia-smi fallback — cache result to avoid per-second subprocess spawn.
        now = time.time()
        if self._gpu_cache is not None and now < self._gpu_cache_exp:
            return self._gpu_cache

        if shutil.which("nvidia-smi"):
            result = self._gpu_via_nvidiasmi()
            if result is not None:
                self._gpu_cache     = result
                self._gpu_cache_exp = now + self.GPU_CACHE_TTL
                return result

        return 0, 0

    def _gpu_via_gputil(self):
        try:
            gpus = GPUtil.getGPUs()
            if not gpus:
                return None
            g = gpus[0]
            gpu  = int(max(0, min(100, g.load * 100)))
            vram = int(max(0, min(100, (g.memoryUsed / g.memoryTotal) * 100))) if g.memoryTotal > 0 else 0
            return gpu, vram
        except Exception:
            return None

    def _gpu_via_nvidiasmi(self):
        try:
            result = subprocess.run(
                ["nvidia-smi", "--query-gpu=utilization.gpu,memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=1.5, check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None

        if result.returncode != 0:
            return None

        line = next((ln.strip() for ln in result.stdout.splitlines() if ln.strip()), "")
        if not line:
            return None

        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3:
            return None

        try:
            gpu        = int(float(parts[0]))
            vram_used  = float(parts[1])
            vram_total = float(parts[2])
        except ValueError:
            return None

        vram = int(max(0, min(100, (vram_used / vram_total) * 100))) if vram_total > 0 else 0
        return int(max(0, min(100, gpu))), vram
