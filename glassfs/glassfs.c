#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/security.h>

#include <asm/uaccess.h>

/* some random number */
#define GLASSFS_MAGIC	0x28476c61
#define FUSE_MAGIC 0x65735546

#define AVFS_MAGIC_CHAR '#'
#define OVERLAY_DIR "/overlay"
#define OVERLAY_DIR_LEN 8

struct base_entry {
	struct dentry *dentry;
	struct vfsmount *mnt;
	int is_avfs;
};

/* Rules for storing underlying filesystem data:
 *
 *  dentry->d_fsdata:     contains the underlying dentry 
 *  inode->u.generic_ip:  contains a kmalloced base_entry struct 
 *  sb->s_fs_info:        contains the underlying superblock
 */

static struct super_operations glassfs_ops;
static struct inode_operations glassfs_file_inode_operations;
static struct file_operations glassfs_file_operations;
static struct inode_operations glassfs_dir_inode_operations;
static struct inode_operations glassfs_symlink_inode_operations;
static struct dentry_operations glassfs_dentry_operations;

static struct inode *glassfs_alloc_inode(struct super_block *sb)
{
	struct base_entry *be;
	struct inode * inode = new_inode(sb);
	if (!inode)
		return NULL;
	
	be = kmalloc(sizeof(struct base_entry), GFP_KERNEL);
	if (!be) {
		iput(inode);
		return NULL;
	}
	inode->u.generic_ip = be;
	be->mnt = NULL;
	be->dentry = NULL;
	
	return inode;
}

static void glassfs_fill_inode(struct inode *inode, int mode, dev_t dev,
			       struct dentry *ndentry, struct vfsmount *nmnt,
			       int is_avfs)
{
	struct base_entry *be = inode->u.generic_ip;

	be->dentry = dget(ndentry);
	be->mnt =  mntget(nmnt);
	be->is_avfs = is_avfs;

	inode->i_mode = mode;
	inode->i_fop = &glassfs_file_operations;
	
	switch (mode & S_IFMT) {
	default:
		inode->i_op = &glassfs_file_inode_operations;
		break;
	case S_IFDIR:
		inode->i_op = &glassfs_dir_inode_operations;
		break;
	case S_IFLNK:
		inode->i_op = &glassfs_symlink_inode_operations;
		break;
	}
}

static void change_list(struct list_head *oldlist, struct list_head *newlist)
{
	struct list_head *prev = oldlist->prev;
	struct list_head *next = oldlist->next;
	prev->next = newlist;
	next->prev = newlist;
}

static void exchange_lists(struct list_head *list1, struct list_head *list2)
{
	change_list(list1, list2);
	change_list(list2, list1);
}

static void exchange_files(struct file *file1, struct file *file2)
{
	struct file tmp;
	
	exchange_lists(&file1->f_list, &file2->f_list);
	exchange_lists(&file1->f_ep_links, &file2->f_ep_links);

	tmp = *file1;
	*file1 = *file2;
	*file2 = tmp;
}

static int glassfs_open(struct inode *inode, struct file *file)
{
	struct base_entry *be = inode->u.generic_ip;
	struct vfsmount *nmnt = mntget(be->mnt);
	struct dentry *ndentry = dget(be->dentry);
	struct file *nfile;
//	printk("glassfs_open\n");
	nfile = dentry_open(ndentry, nmnt, file->f_flags);
	if(IS_ERR(nfile))
		return PTR_ERR(nfile);
	exchange_files(file, nfile);
	fput(nfile);
	return 0;
}

static int is_avfs(const unsigned char *name, unsigned int len)
{
	for (; len--; name++)
		if (*name == (unsigned char) AVFS_MAGIC_CHAR)
			return 1;
	return 0;
}

static int lookup_avfs(struct dentry *dentry, struct nameidata *nd,
		       struct vfsmount *nmnt, struct base_entry *be)
{
	int err;
	char *page;
	char *path;
	
	err = -ENOMEM;
	page = (char *) __get_free_page(GFP_KERNEL);
	if (page) {
		struct nameidata avfsnd;
		unsigned int offset = PAGE_SIZE - dentry->d_name.len - 2;
		path = d_path(nd->dentry,nd->mnt, page, offset);
		err = -ENAMETOOLONG;
		if (!IS_ERR(path) && page + OVERLAY_DIR_LEN < path) {
			unsigned int pathlen = strlen(path);
			path[pathlen] = '/';
			memcpy(path + pathlen + 1, dentry->d_name.name,
			       dentry->d_name.len);
			path[1 + pathlen + dentry->d_name.len] = '\0';
			path -= OVERLAY_DIR_LEN;
			memcpy(path, OVERLAY_DIR, OVERLAY_DIR_LEN);

			printk("lookup_avfs: '%s'\n", path);
			avfsnd.last_type = LAST_ROOT;
			avfsnd.flags = 0;
			avfsnd.mnt = mntget(nmnt);
			avfsnd.dentry = dget(nmnt->mnt_sb->s_root);

			err = path_walk(path, &avfsnd);
			if (!err) {
				if (avfsnd.dentry->d_sb->s_magic != FUSE_MAGIC) {
					printk("glassfs: not an AVFS inode\n");
					path_release(&avfsnd);
					err = -ENOENT;
				} else {
					be->mnt = avfsnd.mnt;
					be->dentry = avfsnd.dentry;
				}
			}
		}
		free_page((unsigned long) page);
	}
	return err;
}

static inline int is_create(struct nameidata *nd)
{
	if (!nd)
		return 1;
	if ((nd->flags & LOOKUP_CREATE) && !(nd->flags & LOOKUP_CONTINUE))
		return 1;
	return 0;
}

static struct dentry *glassfs_lookup(struct inode *dir, struct dentry *dentry,
				    struct nameidata *nd)
{
	struct base_entry *be = dir->u.generic_ip;
	struct dentry *ndentry;
	struct vfsmount *nmnt = mntget(be->mnt);
	struct inode *inode = NULL;
	int curravfs = be->is_avfs;

//	printk("glassfs_lookup\n");
	down(&be->dentry->d_inode->i_sem);
	ndentry = lookup_hash(&dentry->d_name, be->dentry);
	up(&be->dentry->d_inode->i_sem);
	if(!is_create(nd) && !ndentry->d_inode && !curravfs && 
	   is_avfs(dentry->d_name.name, dentry->d_name.len)) {
		struct base_entry avfsbe;
		int err;
		int total_link_count_save = current->total_link_count;
		err = lookup_avfs(dentry, nd, nmnt, &avfsbe);
		current->total_link_count = total_link_count_save;
		if (!err) {
			if (avfsbe.dentry->d_inode) {
				dput(ndentry);
				mntput(nmnt);
				ndentry = avfsbe.dentry;
				nmnt = avfsbe.mnt;
				curravfs = 1;
			} else {
				dput(avfsbe.dentry);
				mntput(avfsbe.mnt);
			}
		}
	}

	if (ndentry->d_inode) {
		struct inode *ninode = ndentry->d_inode;
		inode = glassfs_alloc_inode(dir->i_sb);
		if (!inode) {
			dput(ndentry);
			mntput(nmnt);
			return ERR_PTR(-ENOMEM);
		}
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   ndentry, nmnt, curravfs);
	}

	dentry->d_fsdata = ndentry;
	dentry->d_op = &glassfs_dentry_operations;
	mntput(nmnt);
	
	return d_splice_alias(inode, dentry);
}


static int glassfs_permission(struct inode *inode, int mask,
			   struct nameidata *nd)
{
	struct base_entry *be = inode->u.generic_ip;
	struct inode *ninode = be->dentry->d_inode;
//	printk("glassfs_permission\n");
	return permission(ninode, mask, NULL);
}

/* FIXME: don't use dentry->d_fsdata in create... */

static int glassfs_create(struct inode *dir, struct dentry *dentry,
			  int mode, struct nameidata *nd)
{
	int err;
	struct base_entry *be = dir->u.generic_ip;
	struct dentry *ndentry = dentry->d_fsdata;
	struct inode *ndir = be->dentry->d_inode;
	struct inode *inode = glassfs_alloc_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;

//	printk("glassfs_create\n");
	down(&ndir->i_sem);
	err = vfs_create(ndir, ndentry, mode, NULL);
	up(&ndir->i_sem);
	if (!err && ndentry->d_inode) {
		struct inode *ninode = ndentry->d_inode;
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   ndentry, be->mnt, be->is_avfs);
		d_instantiate(dentry, inode);
	} else
		iput(inode);

	return err;
}

static int glassfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int err;
	struct base_entry *be = dir->u.generic_ip;
	struct dentry *ndentry = dentry->d_fsdata;
	struct inode *ndir = be->dentry->d_inode;
	struct inode *inode = glassfs_alloc_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;

//	printk("glassfs_mkdir\n");
	down(&ndir->i_sem);
	err = vfs_mkdir(ndir, ndentry, mode);
	up(&ndir->i_sem);
	if (!err && ndentry->d_inode) {
		struct inode *ninode = ndentry->d_inode;
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   ndentry, be->mnt, be->is_avfs);
		d_instantiate(dentry, inode);
	} else
		iput(inode);

	return err;
}

static int glassfs_link(struct dentry *old_dentry, struct inode *dir,
			struct dentry *new_dentry)
{
	int err;
	struct base_entry *be = dir->u.generic_ip;
	struct base_entry *old_be = old_dentry->d_inode->u.generic_ip;
	struct dentry *new_ndentry = new_dentry->d_fsdata;
	struct dentry *old_ndentry = old_dentry->d_fsdata;
	struct inode *ndir = be->dentry->d_inode;
	struct inode *inode;

	if(be->mnt != old_be->mnt)
		return -EXDEV;

	inode = glassfs_alloc_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;

//	printk("glassfs_link\n");
	down(&ndir->i_sem);
	err = vfs_link(old_ndentry, ndir, new_ndentry);
	up(&ndir->i_sem);
	if (!err && new_ndentry->d_inode) {
		struct inode *ninode = new_ndentry->d_inode;
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   new_ndentry, be->mnt, be->is_avfs);
		d_instantiate(new_dentry, inode);
	} else
		iput(inode);

	return err;
}

static int glassfs_symlink(struct inode *dir, struct dentry *dentry,
			   const char *oldname)
{
	int err;
	struct base_entry *be = dir->u.generic_ip;
	struct dentry *ndentry = dentry->d_fsdata;
	struct inode *ndir = be->dentry->d_inode;
	struct inode *inode = glassfs_alloc_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;

//	printk("glassfs_symlink\n");
	down(&ndir->i_sem);
	err = vfs_symlink(ndir, ndentry, oldname);
	up(&ndir->i_sem);
	if (!err && ndentry->d_inode) {
		struct inode *ninode = ndentry->d_inode;
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   ndentry, be->mnt, be->is_avfs);
		d_instantiate(dentry, inode);
	} else
		iput(inode);

	return err;
}

static int glassfs_mknod(struct inode *dir, struct dentry *dentry, int mode,
			 dev_t dev)
{
	int err;
	struct base_entry *be = dir->u.generic_ip;
	struct dentry *ndentry = dentry->d_fsdata;
	struct inode *ndir = be->dentry->d_inode;
	struct inode *inode = glassfs_alloc_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;

//	printk("glassfs_mknod\n");
	down(&ndir->i_sem);
	err = vfs_mknod(ndir, ndentry, mode, dev);
	up(&ndir->i_sem);
	if (!err && ndentry->d_inode) {
		struct inode *ninode = ndentry->d_inode;
		glassfs_fill_inode(inode, ninode->i_mode, ninode->i_rdev,
				   ndentry, be->mnt, be->is_avfs);
		d_instantiate(dentry, inode);
	} else
		iput(inode);
	
	return err;
}

static int glassfs_rename(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
{
	int err;
	struct base_entry *old_be = old_dir->u.generic_ip;
	struct base_entry *new_be = new_dir->u.generic_ip;
	struct dentry *old_ndentry = old_dentry->d_fsdata;
	struct dentry *new_ndentry = new_dentry->d_fsdata;
	struct inode *old_ndir = old_be->dentry->d_inode;
	struct inode *new_ndir = new_be->dentry->d_inode;

	if (old_be->mnt != new_be->mnt)
		return -EXDEV;

	lock_rename(new_be->dentry, old_be->dentry);
	err = vfs_rename(old_ndir, old_ndentry, new_ndir, new_ndentry);
	unlock_rename(new_be->dentry, old_be->dentry);
	
	return err;
}

static int glassfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct base_entry *dir_be = dir->u.generic_ip;
	struct inode *ndir = dir_be->dentry->d_inode;
	struct base_entry *be = dentry->d_inode->u.generic_ip;

//	printk("glassfs_unlink\n");
	down(&ndir->i_sem);
	err = vfs_unlink(ndir, be->dentry);
	up(&ndir->i_sem);
	if (!err)
		dentry->d_inode->i_nlink = 0;
	
	return err;
}

static int glassfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct base_entry *dir_be = dir->u.generic_ip;
	struct inode *ndir = dir_be->dentry->d_inode;
	struct base_entry *be = dentry->d_inode->u.generic_ip;

//	printk("glassfs_rmdir\n");
	down(&ndir->i_sem);
	err = vfs_rmdir(ndir, be->dentry);
	up(&ndir->i_sem);
	if (!err)
		dentry->d_inode->i_nlink = 0;
	return err;
}

static int glassfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat)
{
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	//printk("glassfs_getattr (is avfs: %i)\n", be->is_avfs);
	return vfs_getattr(be->mnt, be->dentry, stat);
}

static int glassfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	int err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct inode *ninode = be->dentry->d_inode;
	
	if(IS_RDONLY(ninode))
		return -EROFS;

	if (IS_IMMUTABLE(ninode) || IS_APPEND(ninode))
		return -EPERM;

	down(&ninode->i_sem);
	err = notify_change(be->dentry, attr);
	up(&ninode->i_sem);
	return err;
}

static int glassfs_setxattr(struct dentry *dentry, const char *name,
			    const void *value, size_t size, int flags)
{
	int err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;
	
	err = -EOPNOTSUPP;
	if(ninode->i_op && ninode->i_op->setxattr) {
		down(&ninode->i_sem);
		err = security_inode_setxattr(ndentry, (char *) name, (void *) value, size, flags);
		if (!err) {
			err = ninode->i_op->setxattr(ndentry, name, value, size, flags);
			if (!err)
				security_inode_post_setxattr(ndentry, (char *) name, (void *) value, size, flags);
		}
		up(&ninode->i_sem);
	}
	return err;
}

static ssize_t glassfs_getxattr(struct dentry *dentry, const char *name, 
			       void *value, size_t size)
{
	ssize_t err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;

	err = -EOPNOTSUPP;
	if (ninode->i_op && ninode->i_op->getxattr) {
		err = security_inode_getxattr(ndentry, (char *) name);
		if (!err)
			err = ninode->i_op->getxattr(ndentry, name, value, size);
	}
	return err;
}
static ssize_t glassfs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	ssize_t err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;

	err = -EOPNOTSUPP;
	if (ninode->i_op && ninode->i_op->listxattr) {
		err = security_inode_listxattr(ndentry);
		if (!err)
			err = ninode->i_op->listxattr(ndentry, list, size);
	}
	return err;
}

static int glassfs_removexattr(struct dentry *dentry, const char *name)
{
	int err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;

	err = -EOPNOTSUPP;
	if (ninode->i_op && ninode->i_op->removexattr) {
		err = security_inode_removexattr(ndentry, (char *) name);
		if (!err) {
			/* strange: the security... usually is within
                           the locked region */
			down(&ninode->i_sem);
			err = ninode->i_op->removexattr(ndentry, name);
			up(&ninode->i_sem);
		}
	}
	return err;
}

static int glassfs_statfs(struct super_block *sb, struct kstatfs *buf)
{
	struct super_block *nsb = sb->s_fs_info;
//	printk("glassfs_statfs\n");
	return vfs_statfs(nsb, buf);
}

static void glassfs_dentry_release(struct dentry *dentry)
{
	struct dentry *ndentry = dentry->d_fsdata;
//	printk("glassfs_dentry_release\n");
	dput(ndentry);
}

static int glassfs_dentry_revalidate(struct dentry *dentry,
				     struct nameidata *nd)
{
	struct dentry *ndentry = dentry->d_fsdata;
	//printk("glassfs_dentry_revalidate %.*s\n", dentry->d_name.len, dentry->d_name.name);

	if (d_unhashed(ndentry))
		return 0;

	if (nd && !dentry->d_inode &&
	   is_avfs(dentry->d_name.name, dentry->d_name.len))
	   return 0;

	if (ndentry->d_op && ndentry->d_op->d_revalidate)
		return ndentry->d_op->d_revalidate(ndentry, NULL);
	return 1;
}

static	int glassfs_readlink (struct dentry *dentry, char __user *buf,
			      int buflen)
{

	int err;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;
	
	err = -EINVAL;
	if (ninode->i_op && ninode->i_op->readlink) {
		err = security_inode_readlink(ndentry);
		if (!err) {
			update_atime(ninode);
			err = ninode->i_op->readlink(ndentry, buf, buflen);
		}
	}
	return err;
}

static int glassfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int err = 0;
	struct base_entry *be = dentry->d_inode->u.generic_ip;
	struct dentry *ndentry = be->dentry;
	struct inode *ninode = ndentry->d_inode;
	
	if (ninode->i_op && ninode->i_op->follow_link) {
		err = security_inode_follow_link(ndentry, nd);
		if (!err) {
			update_atime(ninode);
			err = ninode->i_op->follow_link(ndentry, nd);
		}
	}
	return err;
}

static void glassfs_clear_inode(struct inode *inode)
{
	struct base_entry *be = inode->u.generic_ip;
	//printk("glassfs_clear_inode\n");
	dput(be->dentry);
	mntput(be->mnt);
	kfree(be);
}

static struct dentry_operations glassfs_dentry_operations = {
	.d_revalidate	= glassfs_dentry_revalidate,
	.d_release      = glassfs_dentry_release,
};

static struct file_operations glassfs_file_operations = {
	.open		= glassfs_open,
};

static struct inode_operations glassfs_dir_inode_operations = {
	.create		= glassfs_create,
	.lookup		= glassfs_lookup,
	.link		= glassfs_link,
	.unlink		= glassfs_unlink,
	.symlink	= glassfs_symlink,
	.mkdir		= glassfs_mkdir,
	.rmdir		= glassfs_rmdir,
	.mknod		= glassfs_mknod,
	.rename		= glassfs_rename,
	.permission	= glassfs_permission,
	.getattr	= glassfs_getattr,
	.setattr	= glassfs_setattr,
	.setxattr	= glassfs_setxattr,
	.getxattr	= glassfs_getxattr,
	.listxattr	= glassfs_listxattr,
	.removexattr	= glassfs_removexattr,
};

static struct inode_operations glassfs_file_inode_operations = {
	.permission	= glassfs_permission,
	.getattr	= glassfs_getattr,
	.setattr	= glassfs_setattr,
	.setxattr	= glassfs_setxattr,
	.getxattr	= glassfs_getxattr,
	.listxattr	= glassfs_listxattr,
	.removexattr	= glassfs_removexattr,
};

static struct inode_operations glassfs_symlink_inode_operations = {
	.readlink	= glassfs_readlink,
	.follow_link	= glassfs_follow_link,
	.permission	= glassfs_permission,
	.getattr	= glassfs_getattr,
	.setattr	= glassfs_setattr,
	.setxattr	= glassfs_setxattr,
	.getxattr	= glassfs_getxattr,
	.listxattr	= glassfs_listxattr,
	.removexattr	= glassfs_removexattr,
};


static struct super_operations glassfs_ops = {
	.statfs		= glassfs_statfs,
	.clear_inode	= glassfs_clear_inode,
};


static int glassfs_fill_super(struct super_block * sb, void * data, int silent)
{
	struct inode * inode;
	struct dentry * root;
	struct super_block *nsb;
	struct dentry * nroot;
	struct vfsmount *nmnt;

	inode = glassfs_alloc_inode(sb);
	if (!inode)
		return -ENOMEM;
	read_lock(&current->fs->lock);
	nroot = current->fs->root;
	nmnt = current->fs->rootmnt;
	glassfs_fill_inode(inode, S_IFDIR | 0755, 0, nroot, nmnt, 0);
	read_unlock(&current->fs->lock);
	
	nsb = nmnt->mnt_sb;
	sb->s_blocksize = nsb->s_blocksize;
	sb->s_blocksize_bits = nsb->s_blocksize_bits;
	sb->s_magic = GLASSFS_MAGIC;
	sb->s_op = &glassfs_ops;

	root = d_alloc_root(inode);
	if (!root) {
		iput(inode);
		return -ENOMEM;
	}
	sb->s_fs_info = nsb;
	root->d_fsdata = dget(nroot);
	root->d_op = &glassfs_dentry_operations;

	sb->s_root = root;
	return 0;
}

static struct super_block *glassfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
//	printk("glassfs_get_sb\n");
	return get_sb_nodev(fs_type, flags, data, glassfs_fill_super);
}

static struct file_system_type glassfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "glassfs",
	.get_sb		= glassfs_get_sb,
	.kill_sb	= kill_anon_super,
};

static int __init init_glassfs_fs(void)
{
	return register_filesystem(&glassfs_fs_type);
}

static void __exit exit_glassfs_fs(void)
{
	unregister_filesystem(&glassfs_fs_type);
}


module_init(init_glassfs_fs)
module_exit(exit_glassfs_fs)


MODULE_LICENSE("GPL");

/* 
 * Local Variables:
 * indent-tabs-mode: t
 * c-basic-offset: 8
 * End:
 */
