#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include "hideproc.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *) hook->orig) = hook->address;
    return 0;
}

static void notrace hook_ftrace_thunk(unsigned long ip,
                                      unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long) hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{
    int err = hook_resolve_addr(hook);
    if (err)
        return err;

    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}

void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}

typedef struct {
    pid_t id;
    struct list_head list_node;
} pid_node_t;

LIST_HEAD(hidden_proc);

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);
static find_ge_pid_func real_find_ge_pid;

static bool is_hidden_proc(pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        if (proc->id == pid)
            return true;
    }
    return false;
}

static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid = real_find_ge_pid(nr, ns);
    while (pid && is_hidden_proc(pid->numbers->nr))
        pid = real_find_ge_pid(pid->numbers->nr + 1, ns);
    return pid;
}

static struct ftrace_hook hook =
    HOOK("find_ge_pid", hook_find_ge_pid, &real_find_ge_pid);

static void init_hook(void)
{
    hook_install(&hook);
}
static int do_hide_process(pid_t pid)
{
    printk(KERN_INFO "@ %s pid: %d\n", __func__, pid);
    pid_node_t *proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
    if (!proc) {
        printk(KERN_ERR "%s: kmalloc() failed!\n", __func__);
        return -ENOMEM;
    }
    proc->id = pid;
    list_add_tail(&proc->list_node, &hidden_proc);
    return SUCCESS;
}

static pid_t get_parent_pid(pid_t vnr)
{
    struct pid *pid;
    struct task_struct *p;
    pid_t ppid = 0;
    /* Find parent pid */
    pid = find_get_pid(vnr);
    if (!pid) {
        goto out_err;
    }
    p = get_pid_task(pid, PIDTYPE_PID);
    if (!p) {
        goto out_pid;
    }
    ppid = task_pid_vnr(p->real_parent);
    if (!ppid) {
        goto out_task;
    }
out_task:
    put_task_struct(p);
out_pid:
    put_pid(pid);
out_err:
    return ppid;
}

static int hide_process(pid_t vnr)
{
    pid_t ppid;
    int err = SUCCESS;

    if (!vnr)
        return -EAGAIN;
    err = do_hide_process(vnr);
    if (err) {
        return err;
    }

    ppid = get_parent_pid(vnr);
    if (!ppid)
        return -ESRCH;
    err = do_hide_process(ppid);
    if (err)
        return err;
    return err;
}

static void release_hide_list(void)
{
    pid_node_t *proc, *tmp_proc;
    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        list_del(&proc->list_node);
        kfree(proc);
    }
}

static int do_unhide_process(pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        if (proc->id == pid) {
            list_del(&proc->list_node);
            kfree(proc);
        }
    }
    return SUCCESS;
}

static int unhide_process(pid_t pid)
{
    pid_t ppid;
    int err = SUCCESS;
    err = do_unhide_process(pid);
    if (err)
        return err;
    ppid = get_parent_pid(pid);
    if (!ppid)
        return -ESRCH;
    err = do_unhide_process(ppid);
    return err;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static int device_open(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static ssize_t device_read(struct file *filep,
                           char *buffer,
                           size_t len,
                           loff_t *offset)
{
    pid_node_t *proc, *tmp_proc;
    char message[MAX_MESSAGE_SIZE];
    if (*offset)
        return 0;

    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        memset(message, 0, MAX_MESSAGE_SIZE);
        sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
        copy_to_user(buffer + *offset, message, strlen(message));
        *offset += strlen(message);
    }
    return *offset;
}

static ssize_t device_write(struct file *filep,
                            const char *buffer,
                            size_t len,
                            loff_t *offset)
{
    long pid;
    char *message;

    char add_message[] = "add", del_message[] = "del";
    if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
        return -EAGAIN;

    message = kmalloc(len + 1, GFP_KERNEL);
    memset(message, 0, len + 1);
    copy_from_user(message, buffer, len);
    if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
        kstrtol(message + sizeof(add_message), 10, &pid);
        hide_process(pid);
    } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
        kstrtol(message + sizeof(del_message), 10, &pid);
        unhide_process(pid);
    } else {
        kfree(message);
        return -EAGAIN;
    }

    *offset = len;
    kfree(message);
    return len;
}

static struct cdev cdev;
static struct class *hideproc_class = NULL;
static dev_t dev;

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_VERSION 1
#define DEVICE_NAME "hideproc"

static int _hideproc_init(void)
{
    int err;
    struct device *device;

    printk(KERN_INFO "@ %s\n", __func__);
    err = alloc_chrdev_region(&dev, 0, MINOR_VERSION, DEVICE_NAME);
    if (err) {
        printk(KERN_ERR "hideproc: Couldn't alloc_chrdev_region, error=%d\n",
               err);
        goto out_chrdev;
    }

    hideproc_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(hideproc_class)) {
        err = PTR_ERR(hideproc_class);
        printk(KERN_ERR "hideproc: Couldn't class_create, error=%d\n", err);
        goto out_class;
    }

    cdev_init(&cdev, &fops);
    err = cdev_add(&cdev, dev, 1);
    if (err) {
        printk(KERN_ERR "hideproc: Couldn't cdev_add, error=%d\n", err);
        goto out_cdev;
    }
    device = device_create(hideproc_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(device)) {
        err = PTR_ERR(device);
        goto out_device;
    }

    init_hook();

    return 0;

out_device:
    cdev_del(&cdev);
out_cdev:
    class_destroy(hideproc_class);
out_class:
    unregister_chrdev_region(dev, MINOR_VERSION);
out_chrdev:
    return err;
}

static void _hideproc_exit(void)
{
    printk(KERN_INFO "@ %s\n", __func__);
    /* FIXME: ensure the release of all allocated resources */
    release_hide_list();
    hook_remove(&hook);
    device_destroy(hideproc_class, dev);
    cdev_del(&cdev);
    class_destroy(hideproc_class);
    unregister_chrdev_region(dev, MINOR_VERSION);
}

module_init(_hideproc_init);
module_exit(_hideproc_exit);