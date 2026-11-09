#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

struct event_t {
    u32 pid;
    u32 ppid;
    char comm[TASK_COMM_LEN];
    char filename[256];
};

BPF_PERF_OUTPUT(events);

int trace_execve(struct pt_regs *ctx, const char __user *filename,
                 const char __user *const __user *argv,
                 const char __user *const __user *envp) {
    struct event_t evt = {};
    evt.pid = bpf_get_current_pid_tgid() >> 32;
    
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    evt.ppid = task->real_parent->pid;
    
    bpf_get_current_comm(&evt.comm, sizeof(evt.comm));
    bpf_probe_read_user_str(&evt.filename, sizeof(evt.filename), filename);

    events.perf_submit(ctx, &evt, sizeof(evt));
    return 0;
}