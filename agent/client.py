import ctypes
from bcc import BPF

class Event(ctypes.Structure):
    _fields_ = [
        ("pid", ctypes.c_uint32),
        ("ppid", ctypes.c_uint32),
        ("comm", ctypes.c_char * 16),
        ("filename", ctypes.c_char * 256),
    ]

def print_event(cpu, data, size):
    event = ctypes.cast(data, ctypes.POINTER(Event)).contents
    comm = event.comm.decode("utf-8", "ignore").strip("\x00")
    filename = event.filename.decode("utf-8", "ignore").strip("\x00")
    print(f"[EXEC] PID : {event.pid} | PPID : {event.ppid} | Comm : {comm} | Fichier : {filename}")

if __name__ == "__main__":
    # Chargement du fichier C externe depuis le dossier bpf/
    b = BPF(src_file="bpf/probe.c")
    b.attach_kprobe(event=b.get_syscall_fnname("execve"), fn_name="trace_execve")
    b["events"].open_perf_buffer(print_event)

    print("Écoute eBPF active (sudo requis)...")
    try:
        while True:
            b.perf_buffer_poll(timeout=100)
    except KeyboardInterrupt:
        print("\nArrêt.")