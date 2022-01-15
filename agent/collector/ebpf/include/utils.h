// #include "common.h"
#include "bpf_helpers.h"
#include "bpf_core_read.h"
#include "bpf_endian.h"
#include "define.h"
#include "helpers.h"
#include <linux/sched.h>
#include <linux/fdtable.h>

/* save envp to buf (with specific fields) */
// TODO: 后期改成动态的
/* R3 max value is outside of the array range */
// 这个地方非常非常的坑，都因为 bpf_verifier 机制, 之前 buf_off > MAX_PERCPU_BUFSIZE - sizeof(int) 本身都是成立的
// 前面明明有一个更为严格的 data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int)，但是不行
// 在每次调 index 之前都需要 check 一下，所以看源码的时候很多地方会写：To satisfied the verifier...
// TODO: 写一个文章记录一下这个...

// TODO: 判断 kernel version, 使用 ringbuf, 传输优化

static __always_inline int save_envp_to_buf(event_data_t *data, const char __user *const __user *ptr, u8 index)
{
    // Data saved to submit buf: [index][string count][str1 size][str1][str2 size][str2]...
    u8 elem_num = 0;
    data->submit_p->buf[(data->buf_off) & ((MAX_PERCPU_BUFSIZE)-1)] = index;
    // Save space for number of elements (1 byte): [string count]
    u32 orig_off = data->buf_off+1;
    // update the buf_off
    data->buf_off += 2;
    // flags for collection
    int ssh_connection_flag = 0, ld_preload_flag = 0, ld_library_path_flag = 0, tmp_flag = 0;
    /* Bounded loops are available starting with Linux 5.3, so we had to unroll the for loop at compile time */
    #pragma unroll
    for (int i = 0; i < MAX_STR_ARR_ELEM; i++) {
        const char *argp = NULL;
        /* read to argp and check */
        bpf_probe_read(&argp, sizeof(argp), &ptr[i]);
        if (!argp)
            goto out;
        /* check the available size */
        if (data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int))
            goto out;
        /* read into buf & update the elem_num */
        int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off + sizeof(int)]), MAX_STRING_SIZE, argp);
        if (sz > 0) {
            if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int) - (MAX_STRING_SIZE))
                goto out;
            // Add & ((MAX_PERCPU_BUFSIZE)-1)] for verifier if the index is not checked
            if (ld_preload_flag == 0 && sz > 11 && prefix("LD_PRELOAD=",(char*)&data->submit_p->buf[data->buf_off + sizeof(int)], 11)) {
                ld_preload_flag = 1;
                tmp_flag = 1;
            }

            if (ssh_connection_flag == 0 && sz > 15 && prefix("SSH_CONNECTION=",(char*)&data->submit_p->buf[data->buf_off + sizeof(int)], 15)) {
                ssh_connection_flag = 1;
                tmp_flag = 1;
            }

            if (ld_library_path_flag == 0 && sz > 16 && prefix("LD_LIBRARY_PATH=",(char*)&data->submit_p->buf[data->buf_off + sizeof(int)], 16)) {
                ld_library_path_flag = 1;
                tmp_flag = 1;
            }

            if (tmp_flag == 0) {
                continue;
            } else {
                tmp_flag = 0;
            }

            bpf_probe_read(&(data->submit_p->buf[data->buf_off]), sizeof(int), &sz);
            data->buf_off += sz + sizeof(int);
            elem_num++;
            continue;
        } else {
            goto out;
        }
    }
    char ellipsis[] = "...";
    if (data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int))
        goto out;
    int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off + sizeof(int)]), MAX_STRING_SIZE, ellipsis);
    if (sz > 0) {
        if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int))
            goto out;
        bpf_probe_read(&(data->submit_p->buf[data->buf_off]), sizeof(int), &sz);
        data->buf_off += sz + sizeof(int);
        elem_num++;
    }
out:
    data->submit_p->buf[orig_off & ((MAX_PERCPU_BUFSIZE)-1)] = elem_num;
    data->context.argnum++;
    return 1;
}
/* save str array to buf (like args) */
static __always_inline int save_str_arr_to_buf(event_data_t *data, const char __user *const __user *ptr, u8 index)
{
    u8 elem_num = 0;
    data->submit_p->buf[(data->buf_off) & ((MAX_PERCPU_BUFSIZE)-1)] = index;
    u32 orig_off = data->buf_off+1;
    data->buf_off += 2;
    #pragma unroll
    for (int i = 0; i < MAX_STR_ARR_ELEM; i++) {
        const char *argp = NULL;
        bpf_probe_read(&argp, sizeof(argp), &ptr[i]);
        if (!argp)
            goto out;
        if (data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int))
            goto out;
        int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off + sizeof(int)]), MAX_STRING_SIZE, argp);
        if (sz > 0) {
            if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int))
                goto out;
            bpf_probe_read(&(data->submit_p->buf[data->buf_off]), sizeof(int), &sz);
            data->buf_off += sz + sizeof(int);
            elem_num++;
            continue;
        } else {
            goto out;
        }
    }
    char ellipsis[] = "...";
    if (data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int))
        goto out;
    int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off + sizeof(int)]), MAX_STRING_SIZE, ellipsis);
    if (sz > 0) {
        if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int))
            goto out;
        bpf_probe_read(&(data->submit_p->buf[data->buf_off]), sizeof(int), &sz);
        data->buf_off += sz + sizeof(int);
        elem_num++;
    }
out:
    data->submit_p->buf[orig_off & ((MAX_PERCPU_BUFSIZE)-1)] = elem_num;
    data->context.argnum++;
    return 1;
}

static __always_inline int save_str_to_buf(event_data_t *data, void *ptr, u8 index)
{
    // Data saved to submit buf: [index][size][ ... string ... ]
    // If we don't have enough space - return
    if (data->buf_off > (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int))
        return 0;
    // Save argument index
    data->submit_p->buf[(data->buf_off) & ((MAX_PERCPU_BUFSIZE)-1)] = index;
    // Satisfy validator for probe read
    if ((data->buf_off+1) <= (MAX_PERCPU_BUFSIZE) - (MAX_STRING_SIZE) - sizeof(int)) {
        // Read into buffer
        int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off+1+sizeof(int)]), MAX_STRING_SIZE, ptr);
        if (sz > 0) {
            // Satisfy validator for probe read
            if ((data->buf_off+1) > (MAX_PERCPU_BUFSIZE) - sizeof(int)) {
                return 0;
            }
            __builtin_memcpy(&(data->submit_p->buf[data->buf_off+1]), &sz, sizeof(int));
            data->buf_off += sz + sizeof(int) + 1;
            data->context.argnum++;
            return 1;
        }
    }
    return 0;
}

// TODO: pid lru 加速, 需要考虑到更新的场景, 加入时间 tag
static __always_inline int save_pid_tree_new_to_buf(event_data_t *data, int limit, u8 index)
{
    // data: [index][string count][pid1][str1 size][str1][pid2][str2 size][str2]
    u8 elem_num = 0;
    data->submit_p->buf[(data->buf_off) & ((MAX_PERCPU_BUFSIZE)-1)] = index;
    u32 orig_off = data->buf_off+1;
    data->buf_off += 2;
    // precheck limit: unuseful?
    if (limit < 1 || limit >= 32) {
        return 0;
    }
    struct task_struct *task = data->task;
    u32 pid;
    #pragma unroll
    for (int i = 0; i < limit; i++) {
        // check pid
        int flag = bpf_probe_read(&pid, sizeof(pid), &task->tgid);
        if (flag != 0)
            goto out;
        // trace until pid = 1
        if (pid == 0)
            goto out;
        if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int)) {
            goto out;
        }
        // read pid to buffer firstly
        flag = bpf_probe_read(&(data->submit_p->buf[data->buf_off]), sizeof(int), &pid);
        if (flag != 0)
            goto out;
        // read comm
        if (data->buf_off >= (MAX_PERCPU_BUFSIZE) - (TASK_COMM_LEN) - sizeof(int) - sizeof(int))
            goto out;
        int sz = bpf_probe_read_str(&(data->submit_p->buf[data->buf_off + sizeof(int) + sizeof(int)]), TASK_COMM_LEN, &task->comm);
        if (sz > 0) {
            if (data->buf_off > (MAX_PERCPU_BUFSIZE) - sizeof(int) - sizeof(int))
                goto out;
            bpf_probe_read(&(data->submit_p->buf[data->buf_off + sizeof(int)]), sizeof(int), &sz);
            data->buf_off += sz + sizeof(int) + sizeof(int);
            elem_num++;
            flag = bpf_probe_read(&task, sizeof(task), &task->real_parent);
            if (flag != 0)
                goto out;
            continue;
        } else {
            goto out;
        }
    }
out:
    data->submit_p->buf[orig_off & ((MAX_PERCPU_BUFSIZE)-1)] = elem_num;
    data->context.argnum++;
    return 1;
}
/* init_context */
static __always_inline int init_context(context_t *context, struct task_struct *task) {
    // 获取 timestamp
    struct task_struct * realparent;
    bpf_probe_read(&realparent, sizeof(realparent), &task->real_parent);
    bpf_probe_read(&context->ppid, sizeof(context->ppid), &realparent->tgid);
    context->ts = bpf_ktime_get_ns();
    // 填充 id 相关信息
    u64 id = bpf_get_current_uid_gid();
    context->uid = id;
    context->gid = id >> 32;
    id = bpf_get_current_pid_tgid();
    context->pid = id;
    context->tid = id >> 32;
    context->cgroup_id = bpf_get_current_cgroup_id();
    // 读取 cred 下, 填充 id
    struct cred *cred;
    // 有三个 ptracer_cred, real_cred, cred, 看kernel代码即可
    bpf_probe_read(&cred, sizeof(cred), &task->real_cred);
    bpf_probe_read(&context->euid, sizeof(context->euid), &cred->euid);

    // 容器相关信息
    struct nsproxy *nsp;
    struct uts_namespace *uts_ns;
    bpf_probe_read(&nsp, sizeof(nsp), &task->nsproxy);
    bpf_probe_read(&uts_ns, sizeof(uts_ns), &nsp->uts_ns);
    bpf_probe_read_str(&context->nodename, sizeof(context->nodename), &uts_ns->name.nodename);
    bpf_probe_read(&context->uts_inum, sizeof(context->uts_inum), &uts_ns->ns.inum);
    bpf_probe_read(&nsp, sizeof(nsp), &realparent->nsproxy);
    bpf_probe_read(&uts_ns, sizeof(uts_ns), &nsp->uts_ns);
    bpf_probe_read(&context->parent_uts_inum, sizeof(context->parent_uts_inum), &uts_ns->ns.inum);    
    // sessionid
    bpf_probe_read(&context->sessionid, sizeof(context->sessionid), &task->sessionid);
    struct pid_cache_t * parent = bpf_map_lookup_elem(&pid_cache_lru, &context->pid);
    if (parent) {
        // 感觉没必要，就做 command line 加速吧
        bpf_probe_read(&context->pcomm, sizeof(context->pcomm), &parent->pcomm);
    }
    bpf_get_current_comm(&context->comm, sizeof(context->comm));
    context->argnum = 0;
    return 0;
}

/* ==== get ==== */

static __always_inline void* get_tty_str()
{
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    struct signal_struct *signal;
    bpf_probe_read(&signal, sizeof(signal), &task->signal);
    struct tty_struct *tty;
    bpf_probe_read(&tty, sizeof(tty), &signal->tty);

    // Get per-cpu string buffer
    buf_t *string_p = get_buf(STRING_BUF_IDX);
    if (string_p == NULL)
        return NULL;
    int size = bpf_probe_read_str(&(string_p->buf[0]), 64, &tty->name);
    char nothing[] = "-1";
    if (size <= 1)
        bpf_probe_read_str(&(string_p->buf[0]), 1, nothing);
    return &string_p->buf[0];
}

static __always_inline void* get_path_str(struct path *path)
{
    struct path f_path;
    bpf_probe_read(&f_path, sizeof(struct path), path);
    char slash = '/';
    int zero = 0;
    struct dentry *dentry = f_path.dentry;
    struct vfsmount *vfsmnt = f_path.mnt;
    struct mount *mnt_parent_p;

    struct mount *mnt_p = real_mount(vfsmnt);
    bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);

    u32 buf_off = (MAX_PERCPU_BUFSIZE >> 1);
    struct dentry *mnt_root;
    struct dentry *d_parent;
    struct qstr d_name;
    unsigned int len;
    unsigned int off;
    int sz;

    // Get per-cpu string buffer
    buf_t *string_p = get_buf(STRING_BUF_IDX);
    if (string_p == NULL)
        return NULL;

    #pragma unroll
    for (int i = 0; i < MAX_PATH_COMPONENTS; i++) {
        mnt_root = READ_KERN(vfsmnt->mnt_root);
        d_parent = READ_KERN(dentry->d_parent);
        if (dentry == mnt_root || dentry == d_parent) {
            if (dentry != mnt_root) {
                // We reached root, but not mount root - escaped?
                break;
            }
            if (mnt_p != mnt_parent_p) {
                // We reached root, but not global root - continue with mount point path
                bpf_probe_read(&dentry, sizeof(struct dentry*), &mnt_p->mnt_mountpoint);
                bpf_probe_read(&mnt_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                vfsmnt = &mnt_p->mnt;
                continue;
            }
            // Global root - path fully parsed
            break;
        }
        // Add this dentry name to path
        d_name = READ_KERN(dentry->d_name);
        len = (d_name.len+1) & (MAX_STRING_SIZE-1);
        off = buf_off - len;

        // Is string buffer big enough for dentry name?
        sz = 0;
        if (off <= buf_off) { // verify no wrap occurred
            len = len & (((MAX_PERCPU_BUFSIZE) >> 1)-1);
            sz = bpf_probe_read_str(&(string_p->buf[off & (((MAX_PERCPU_BUFSIZE) >> 1)-1)]), len, (void *)d_name.name);
        }
        else
            break;
        if (sz > 1) {
            buf_off -= 1; // remove null byte termination with slash sign
            bpf_probe_read(&(string_p->buf[buf_off & ((MAX_PERCPU_BUFSIZE)-1)]), 1, &slash);
            buf_off -= sz - 1;
        } else {
            // If sz is 0 or 1 we have an error (path can't be null nor an empty string)
            break;
        }
        dentry = d_parent;
    }

    if (buf_off == (MAX_PERCPU_BUFSIZE >> 1)) {
        // memfd files have no path in the filesystem -> extract their name
        buf_off = 0;
        d_name = READ_KERN(dentry->d_name);
        bpf_probe_read_str(&(string_p->buf[0]), MAX_STRING_SIZE, (void *)d_name.name);
    } else {
        // Add leading slash
        buf_off -= 1;
        bpf_probe_read(&(string_p->buf[buf_off & ((MAX_PERCPU_BUFSIZE)-1)]), 1, &slash);
        // Null terminate the path string
        bpf_probe_read(&(string_p->buf[((MAX_PERCPU_BUFSIZE) >> 1)-1]), 1, &zero);
    }

    set_buf_off(STRING_BUF_IDX, buf_off);
    return &string_p->buf[buf_off];
}

/*test only*/
static __always_inline void* get_path_str_once(struct path *path)
{
    struct path f_path;
    bpf_probe_read(&f_path, sizeof(struct path), path);
    char slash = '/';
    int zero = 0;
    struct dentry *dentry = f_path.dentry;
    struct vfsmount *vfsmnt = f_path.mnt;
    struct mount *mnt_parent_p;
    struct mount *mnt_p = real_mount(vfsmnt);
    bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);
    u32 buf_off = (MAX_PERCPU_BUFSIZE >> 1);
    struct dentry *mnt_root;
    struct dentry *d_parent;
    struct qstr d_name;
    unsigned int len;
    unsigned int off;
    int sz;

    // Get per-cpu string buffer
    buf_t *string_p = get_buf(STRING_BUF_IDX);
    if (string_p == NULL)
        return NULL;

    #pragma unroll
    for (int i = 0; i < 1; i++) {
        mnt_root = READ_KERN(vfsmnt->mnt_root);
        d_parent = READ_KERN(dentry->d_parent);
        if (dentry == mnt_root || dentry == d_parent) {
            if (dentry != mnt_root) {
                // We reached root, but not mount root - escaped?
                break;
            }
            if (mnt_p != mnt_parent_p) {
                // We reached root, but not global root - continue with mount point path
                bpf_probe_read(&dentry, sizeof(struct dentry*), &mnt_p->mnt_mountpoint);
                bpf_probe_read(&mnt_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                vfsmnt = &mnt_p->mnt;
                continue;
            }
            // Global root - path fully parsed
            break;
        }
        // Add this dentry name to path
        d_name = READ_KERN(dentry->d_name);
        len = (d_name.len+1) & (MAX_STRING_SIZE-1);
        off = buf_off - len;

        // Is string buffer big enough for dentry name?
        sz = 0;
        if (off <= buf_off) { // verify no wrap occurred
            len = len & (((MAX_PERCPU_BUFSIZE) >> 1)-1);
            sz = bpf_probe_read_str(&(string_p->buf[off & (((MAX_PERCPU_BUFSIZE) >> 1)-1)]), len, (void *)d_name.name);
        }
        else
            break;
        if (sz > 1) {
            buf_off -= 1; // remove null byte termination with slash sign
            bpf_probe_read(&(string_p->buf[buf_off & ((MAX_PERCPU_BUFSIZE)-1)]), 1, &slash);
            buf_off -= sz - 1;
        } else {
            // If sz is 0 or 1 we have an error (path can't be null nor an empty string)
            break;
        }
        dentry = d_parent;
    }

    if (buf_off == (MAX_PERCPU_BUFSIZE >> 1)) {
        // memfd files have no path in the filesystem -> extract their name
        buf_off = 0;
        d_name = READ_KERN(dentry->d_name);
        bpf_probe_read_str(&(string_p->buf[0]), MAX_STRING_SIZE, (void *)d_name.name);
    } else {
        // Add leading slash
        buf_off -= 1;
        bpf_probe_read(&(string_p->buf[buf_off & ((MAX_PERCPU_BUFSIZE)-1)]), 1, &slash);
        // Null terminate the path string
        bpf_probe_read(&(string_p->buf[((MAX_PERCPU_BUFSIZE) >> 1)-1]), 1, &zero);
    }

    set_buf_off(STRING_BUF_IDX, buf_off);
    return &string_p->buf[buf_off];
}

static __always_inline int get_network_details_from_sock_v4(struct sock *sk, net_conn_v4_t *net_details, int peer)
{
    // struct inet_sock *inet = inet_sk(sk);
    struct inet_sock *inet = (struct inet_sock *)sk;
    if (!peer) {
        net_details->local_address = READ_KERN(inet->inet_rcv_saddr);
        net_details->local_port = bpf_ntohs(READ_KERN(inet->inet_num));
        net_details->remote_address = READ_KERN(inet->inet_daddr);
        net_details->remote_port = READ_KERN(inet->inet_dport);
    }
    else {
        net_details->remote_address = READ_KERN(inet->inet_rcv_saddr);
        net_details->remote_port = bpf_ntohs(READ_KERN(inet->inet_num));
        net_details->local_address = READ_KERN(inet->inet_daddr);
        net_details->local_port = READ_KERN(inet->inet_dport);
    }

    return 0;
}

static __always_inline int get_remote_sockaddr_in_from_network_details(struct sockaddr_in *addr, net_conn_v4_t *net_details, u16 family)
{
    addr->sin_family = family;
    addr->sin_port = net_details->remote_port;
    addr->sin_addr.s_addr = net_details->remote_address;

    return 0;
}

static __always_inline struct file *file_get_raw(u64 fd_num)
{
    // get current task
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    if (task == NULL) {
        return NULL;
    }
    // get files
    struct files_struct *files = (struct files_struct *)READ_KERN(task->files);
    if (files == NULL) {
        return NULL;
    }
    // get fdtable
    struct fdtable *fdt = (struct fdtable *)READ_KERN(files->fdt);
    if (fdt == NULL) {
        return NULL;
    }
    struct file **fd = (struct file **)READ_KERN(fdt->fd);
    if (fd == NULL) {
        return NULL;
    }
    struct file *f = (struct file *)READ_KERN(fd[fd_num]);
    if (f == NULL) {
        return NULL;
    }

    return f;
}
//TODO: op
static __always_inline void *get_fraw_str(u64 num)
{
    char nothing[] = "-1";
    buf_t *string_p = get_buf(STRING_BUF_IDX);
    if (string_p == NULL)
        return NULL;
    // review
    bpf_probe_read_str(&(string_p->buf[0]), 1, nothing);
    struct file *f = file_get_raw(num);
    if (!f)
        return &string_p->buf[0];
    struct path p = READ_KERN(f->f_path);
    void *path = get_path_str(GET_FIELD_ADDR(p));
    if (!path)
        return &string_p->buf[0];
    return path;
}

static __always_inline int save_to_submit_buf(event_data_t *data, void *ptr, u32 size, u8 index)
{
// The biggest element that can be saved with this function should be defined here
#define MAX_ELEMENT_SIZE sizeof(struct sockaddr_un)

    // Data saved to submit buf: [index][ ... buffer[size] ... ]

    if (size == 0)
        return 0;

    // If we don't have enough space - return
    if (data->buf_off > MAX_PERCPU_BUFSIZE - (size+1))
        return 0;

    // Save argument index
    volatile int buf_off = data->buf_off;
    data->submit_p->buf[buf_off & (MAX_PERCPU_BUFSIZE-1)] = index;

    // Satisfy validator for probe read
    if ((data->buf_off+1) <= MAX_PERCPU_BUFSIZE - MAX_ELEMENT_SIZE) {
        // Read into buffer
        if (bpf_probe_read(&(data->submit_p->buf[data->buf_off+1]), size, ptr) == 0) {
            // We update buf_off only if all writes were successful
            data->buf_off += size+1;
            data->context.argnum++;
            return 1;
        }
    }
    return 0;
}

/* Reference: http://jinke.me/2018-08-23-socket-and-linux-file-system/ */
static __always_inline int get_socket_info_sub(event_data_t *data, struct fdtable *fdt, u8 index)
{
    u16 family;
    void *tmp_socket = NULL;
    struct socket *socket;
    struct sock *sk;
    struct file **fd;
    struct file *file;
    int state;
    // 跨 bpf program 做 read 数据传输
    buf_t *string_p = get_buf(STRING_BUF_IDX);
    if (string_p == NULL)
        return -1;
    // get max fds 
    unsigned long max_fds;
    bpf_probe_read(&max_fds, sizeof(max_fds), &fdt->max_fds);
    fd = (struct file **)READ_KERN(fdt->fd);
    if (fd == NULL)
        return -1;
    // unroll since unbounded loop is not supported < kernel version 5.3
    #pragma unroll
    for (int i = 0; i < 8; i++) {
        if (i == max_fds)
            break;
        file = (struct file *)READ_KERN(fd[i]);
        if (file == NULL)
            continue;
        // 获取 path 名, 这个需要大优化, 参考 datadog, 周末安排
        struct path *path;
        struct dentry *dentry; 
        struct qstr d_name;
        bpf_probe_read(&path, sizeof(path), &file->f_path);
        dentry = (struct dentry *)READ_KERN(path->dentry);
        if (!dentry)
            continue;
        d_name = READ_KERN(dentry->d_name);
        /* test code */
        int size = bpf_probe_read_str(&(string_p->buf[0]), 24, (void *)d_name.name);
        if (size != 9) {
            struct sockaddr_in remote;
            remote.sin_family = maxsize_fds;
            save_to_submit_buf(data, &remote, sizeof(struct sockaddr_in), index);
            return 1;
        }
        if (prefix("socket:[", (char *)&string_p->buf[0], 8) == 1) {
            struct sockaddr_in remote;
            remote.sin_family = 3;
            save_to_submit_buf(data, &remote, sizeof(struct sockaddr_in), index);
            return 1;
            // bpf_probe_read(&tmp_socket, sizeof(tmp_socket), &file->private_data);
            // if(!tmp_socket)
            //     continue;
            // socket = (struct socket *)tmp_socket;
            // // check state
            // bpf_probe_read(&state, sizeof(state), &socket->state);
            // if (state <= 1)
            //     continue;
            // bpf_probe_read(&sk, sizeof(sk), &socket->sk);
            // if(!sk)
            //     continue;
            // // 先不支持 IPv6, 跑通先
            // family = READ_KERN(sk->sk_family);
            // if (family == AF_INET) {
            //     net_conn_v4_t net_details = {};
            //     get_network_details_from_sock_v4(sk, &net_details, 1);
            //     // remote we need to send
            //     struct sockaddr_in remote;
            //     get_remote_sockaddr_in_from_network_details(&remote, &net_details, family);
            //     save_to_submit_buf(data, &remote, sizeof(struct sockaddr_in), index);
            //     return 1;
            // }
        }
    }
    return 0;
}

// 向上溯源获取 socket 信息, 参考字节 Elkeid & trace 代码
// 需要做 lru 加速
static __always_inline int get_socket_info(event_data_t *data, u8 index)
{
    struct sockaddr_in remote;
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    if (task == NULL)
        goto exit;
    
    u32 pid;
    int flag;

    #pragma unroll
    for (int i = 0; i < 4; i++) {
        bpf_probe_read(&pid, sizeof(pid), &task->pid);
        // 0 for failed...
        if (pid == 1)
            break;
        
        // get files
        struct files_struct *files = (struct files_struct *)READ_KERN(task->files);
        if (files == NULL)
            goto next_task;
        
        // get fdtable
        struct fdtable *fdt = (struct fdtable *)READ_KERN(files->fdt);
        if (fdt == NULL)
            goto next_task;
        
        // find out
        flag = get_socket_info_sub(data, fdt, index);
        if (flag == 1)
            return 0;
next_task:
        bpf_probe_read(&task, sizeof(task), &task->real_parent);
    }
exit:
    remote.sin_family = 0;
    remote.sin_port = 0;
    remote.sin_addr.s_addr = 0;
    save_to_submit_buf(data, &remote, sizeof(struct sockaddr_in), index);
    return 0;
}

static __always_inline int init_event_data(event_data_t *data, void *ctx)
{
    data->task = (struct task_struct *)bpf_get_current_task();
    init_context(&data->context, data->task);
    data->ctx = ctx;
    data->buf_off = sizeof(context_t);
    int buf_idx = SUBMIT_BUF_IDX;
    data->submit_p = bpf_map_lookup_elem(&bufs, &buf_idx);
    if (data->submit_p == NULL)
        return 0;
    return 1;
}

static __always_inline int events_perf_submit(event_data_t *data) {
    bpf_probe_read(&(data->submit_p->buf[0]), sizeof(context_t), &data->context);
    int size = data->buf_off & (MAX_PERCPU_BUFSIZE-1);
    void *output_data = data->submit_p->buf;
    return bpf_perf_event_output(data->ctx, &exec_events, BPF_F_CURRENT_CPU, output_data, size);
}