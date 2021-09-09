#include <linux/module.h>
#include <linux/fs_context.h>
#include <linux/fs.h>
#include <linux/pseudo_fs.h>

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

static int nvidia_pidns_init(void)
{
	struct vfsmount *mnt = NULL;
	struct inode *inode = NULL;
	struct file *file = NULL;
	struct dentry *dentry = NULL;
	struct path path;
	int ret = 0;

	mnt = kern_mount(&dummy_fs_type);
	if (IS_ERR(mnt)) {
		ret = PTR_ERR(mnt);
		mnt = NULL;
		goto out;
	}

	inode = alloc_anon_inode(mnt->mnt_sb);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		inode = NULL;
		goto out;
	}
	init_special_inode(inode, S_IFCHR | 0666, MKDEV(1, 3));

	dentry = d_alloc_anon(mnt->mnt_sb);
	if (!dentry) {
		ret = -ENOMEM;
		goto out;
	}
	dentry = d_instantiate_anon(dentry, inode);
	inode = NULL;

	path.mnt = mnt;
	path.dentry = dentry;
	file = dentry_open(&path, O_RDWR, current_cred());
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out;
	}

	ret = 0;

out:
	if (file)
		fput(file);
	if (dentry)
		dput(dentry);
	if (inode)
		iput(inode);
	if (mnt)
		kern_unmount(mnt);
	return ret;
}

static void nvidia_pidns_exit(void)
{
}

module_init(nvidia_pidns_init);
module_exit(nvidia_pidns_exit);
MODULE_LICENSE("GPL");

// vim: noet
