#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>

#define NVIDIA_CTL_RDEV (MKDEV(195, 255))
#define MAX_PID_COUNT (4096)

#ifndef __amd64__
#error only amd64 supported
#endif

enum nvidia_fixer_state {
	NVIDIA_FIXER_INIT,
	NVIDIA_FIXER_SUCCESS,
	NVIDIA_FIXER_ERROR,
};

struct nvidia_req_arg {
	u32 rsv0;
	u32 rsv1;
	u32 version;
	u32 rsv3;
	void __user *data;
	u32 cmd;
};

struct nvidia_pidns_call;
typedef int (*nvidia_fixer_t)(struct nvidia_pidns_call *, enum nvidia_fixer_state);

struct nvidia_pidns_call {
	nvidia_fixer_t fixer;
	void *priv;
	void __user *data;
};

static struct file *nvidia_ctl;
static long (*nvidia_orig_unlocked_ioctl)(struct file *, unsigned int, unsigned long);
static long (*nvidia_orig_compat_ioctl)(struct file *, unsigned int, unsigned long);

static int dummy_fs_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, 0xd09858b3) ? 0 : -ENOMEM;
}

static struct file_system_type dummy_fs_type = {
	.owner = THIS_MODULE,
	.name = "nvidia_pidns",
	.init_fs_context = dummy_fs_init_fs_context,
	.kill_sb = kill_anon_super,
};

static int fixer_0x0ee4(struct nvidia_pidns_call *call, enum nvidia_fixer_state st)
{
	u32 pid_count;
	u32 *pid_items = NULL;

	u32 wr, rd, i;
	int ret = 0;

	switch (st) {
		case NVIDIA_FIXER_INIT:
			rcu_read_lock();
			break;
		case NVIDIA_FIXER_ERROR:
			rcu_read_unlock();
			break;
		case NVIDIA_FIXER_SUCCESS:
			if (copy_from_user(&pid_count, call->data + 8, sizeof(u32))) {
				ret = -EFAULT;
				goto out;
			}
			if (pid_count > MAX_PID_COUNT) {
				ret = -EOVERFLOW;
				goto out;
			}

			pid_items = kmalloc(pid_count * sizeof(u32), GFP_KERNEL);
			if (!pid_items) {
				ret = -ENOMEM;
				goto out;
			}

			if (copy_from_user(pid_items, call->data + 12, pid_count * sizeof(u32))) {
				ret = -EFAULT;
				goto out;
			}

			/* translate PIDs to current namespace */
			for (wr = rd = 0; rd < pid_count; rd++) {
				struct pid *pid = find_pid_ns(pid_items[rd], &init_pid_ns);
				u32 vpid = pid ? pid_vnr(pid) : 0;
				if (vpid)
					pid_items[wr++] = vpid;
			}

			/* clear entries after end */
			for (i = wr; i < pid_count; i++)
				pid_items[i] = 0;

			/* copy results back to userspace */
			if (copy_to_user(call->data + 8, &wr, sizeof(u32))) {
				ret = -EFAULT;
				goto out;
			}
			if (copy_to_user(call->data + 12, pid_items, pid_count * sizeof(u32))) {
				ret = -EFAULT;
				goto out;
			}

out:
			rcu_read_unlock();
			kfree(pid_items);
			break;
		default:
			BUG();
			break;
	}

	return ret;
}

static int fixer_0x2588(struct nvidia_pidns_call *call, enum nvidia_fixer_state st)
{
	u32 pid_count;
	u32 *orig_pids = (st == NVIDIA_FIXER_INIT) ? NULL : call->priv;

	u32 i;
	int ret = 0;

	if (copy_from_user(&pid_count, call->data, sizeof(u32))) {
		ret = -EFAULT;
		goto out;
	}
	if (pid_count > MAX_PID_COUNT) {
		ret = -EOVERFLOW;
		goto out;
	}

	switch (st) {
		case NVIDIA_FIXER_INIT:
			/* save original PIDs */
			orig_pids = kmalloc(pid_count * sizeof(u32), GFP_KERNEL);
			if (!orig_pids) {
				ret = -ENOMEM;
				goto out;
			}
			for (i = 0; i < pid_count; i++) {
				if (copy_from_user(&orig_pids[i], call->data + 0x8 + 0x30 * i, sizeof(u32))) {
					ret = -EFAULT;
					goto out;
				}
			}

			/* translate the PIDs */
			rcu_read_lock();
			for (i = 0; i < pid_count; i++) {
				struct pid *pid = find_vpid(orig_pids[i]);
				u32 ipid = pid ? pid_nr(pid) : 0;
				if (copy_to_user(call->data + 0x8 + 0x30 * i, &ipid, sizeof(u32))) {
					ret = -EFAULT;
					break; /* check ret after leaving loop */
				}
			}
			rcu_read_unlock();

			if (ret)
				goto out;

			/* save original PIDs into priv */
			call->priv = orig_pids;
			orig_pids = NULL;
			break;
		case NVIDIA_FIXER_ERROR:
		case NVIDIA_FIXER_SUCCESS:
			/* restore original PIDs */
			for (i = 0; i < pid_count; i++) {
				if (copy_to_user(call->data + 0x8 + 0x30 * i, &orig_pids[i], sizeof(u32))) {
					ret = -EFAULT;
					break;
				}
			}
			break;
	}

out:
	kfree(orig_pids);
	return ret;
}

static long fix_before_call(struct nvidia_pidns_call *call, struct file *f, unsigned int cmd, unsigned long ularg)
{
	struct nvidia_req_arg arg;

	call->fixer = NULL;

	if (file_inode(f)->i_rdev != NVIDIA_CTL_RDEV) {
		return 0;
	}

	if (cmd != _IOC(_IOC_READ | _IOC_WRITE, 'F', 0x2a, sizeof(arg))) {
		return 0;
	}

	if (copy_from_user(&arg, (void __user *)ularg, sizeof(arg))) {
		return -EFAULT;
	}

#define VERSIONED_CMD(v, c) ((u64)(v) << 32 | (u64)(c))
	switch (VERSIONED_CMD(arg.version, arg.cmd)) {
		case VERSIONED_CMD(0x2080018d, 0x0ee4):
			call->fixer = fixer_0x0ee4;
			break;
		case VERSIONED_CMD(0x2080018e, 0x2588):
			call->fixer = fixer_0x2588;
			break;
		default:
			return 0;
	}
#undef VERSIONED_CMD

	call->data = arg.data;
	call->priv = NULL;
	return call->fixer(call, NVIDIA_FIXER_INIT);
}

static long fix_after_call(struct nvidia_pidns_call *call, long ret)
{
	if (call->fixer) {
		if (ret == 0)
			ret = call->fixer(call, NVIDIA_FIXER_SUCCESS);
		else
			call->fixer(call, NVIDIA_FIXER_ERROR);
	}
	return ret;
}

static long nvidia_pidns_unlocked_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct nvidia_pidns_call call;
	long ret;

	ret = fix_before_call(&call, f, cmd, arg);
	if (ret == 0) {
		ret = nvidia_orig_unlocked_ioctl(f, cmd, arg);
		ret = fix_after_call(&call, ret);
	}

	return ret;
}

static long nvidia_pidns_compat_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct nvidia_pidns_call call;
	long ret;

	ret = fix_before_call(&call, f, cmd, arg);
	if (ret == 0) {
		ret = nvidia_orig_compat_ioctl(f, cmd, arg);
		ret = fix_after_call(&call, ret);
	}

	return ret;
}

static struct file *find_nvidia_ctl(void)
{
	struct vfsmount *mnt = NULL;
	struct inode *inode = NULL;
	struct file *file = NULL;
	struct dentry *dentry = NULL;
	struct path path;

	mnt = kern_mount(&dummy_fs_type);
	if (IS_ERR(mnt)) {
		file = ERR_CAST(mnt);
		mnt = NULL;
		goto out;
	}

	inode = alloc_anon_inode(mnt->mnt_sb);
	if (IS_ERR(inode)) {
		file = ERR_CAST(inode);
		inode = NULL;
		goto out;
	}
	init_special_inode(inode, S_IFCHR | 0666, NVIDIA_CTL_RDEV);

	dentry = d_alloc_anon(mnt->mnt_sb);
	if (!dentry) {
		file = ERR_PTR(-ENOMEM);
		goto out;
	}
	dentry = d_instantiate_anon(dentry, inode);
	inode = NULL;

	path.mnt = mnt;
	path.dentry = dentry;
	file = dentry_open(&path, O_RDWR, current_cred());

	if (IS_ERR(file))
		pr_err("nvidia-pidns: failed to open nvidiactl (%ld), "
				"is the nvidia module loaded?\n", PTR_ERR(file));

out:
	if (dentry)
		dput(dentry);
	if (inode)
		iput(inode);
	if (mnt)
		kern_unmount(mnt);
	return file;
}

static int nvidia_pidns_init(void)
{
	struct file_operations *fops;
	struct file *f = find_nvidia_ctl();

	if (IS_ERR(f))
		return PTR_ERR(f);

	nvidia_ctl = f;
	fops = (struct file_operations *)f->f_op;

	/* Save original callbacks so we can use them later */
	nvidia_orig_unlocked_ioctl = fops->unlocked_ioctl;
	nvidia_orig_compat_ioctl = fops->compat_ioctl;

	/* Replace with our wrappers */
	wmb();
	fops->unlocked_ioctl = nvidia_pidns_unlocked_ioctl;
	fops->compat_ioctl = nvidia_pidns_compat_ioctl;

	return 0;
}

static void nvidia_pidns_exit(void)
{
	struct file *f = nvidia_ctl;
	struct file_operations *fops = (struct file_operations *)f->f_op;

	fops->unlocked_ioctl = nvidia_orig_unlocked_ioctl;
	fops->compat_ioctl = nvidia_orig_compat_ioctl;
	wmb();

	fput(nvidia_ctl);
}

module_init(nvidia_pidns_init);
module_exit(nvidia_pidns_exit);
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: nvidia");

// vim: noet
