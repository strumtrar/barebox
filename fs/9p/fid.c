// SPDX-License-Identifier: GPL-2.0-only
/*
 * V9FS FID Management
 *
 *  Copyright (C) 2007 by Latchesar Ionkov <lucho@ionkov.net>
 *  Copyright (C) 2005, 2006 by Eric Van Hensbergen <ericvh@gmail.com>
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>

#include "v9fs.h"
#include "v9fs_vfs.h"
#include "fid.h"

static inline void __add_fid(struct dentry *dentry, struct p9_fid *fid)
{
	hlist_add_head(&fid->dlist, (struct hlist_head *)&dentry->d_fsdata);
}

/**
 * v9fs_fid_add - add a fid to a dentry
 * @dentry: dentry that the fid is being added to
 * @pfid: fid to add, NULLed out
 *
 */
void v9fs_fid_add(struct dentry *dentry, struct p9_fid **pfid)
{
	struct p9_fid *fid = *pfid;

	spin_lock(&dentry->d_lock);
	__add_fid(dentry, fid);
	spin_unlock(&dentry->d_lock);

	*pfid = NULL;
}

/**
 * v9fs_fid_find_inode - search for an open fid off of the inode list
 * @inode: return a fid pointing to a specific inode
 * @want_writeable: only consider fids which are writeable
 * @uid: return a fid belonging to the specified user
 * @any: ignore uid as a selection criteria
 *
 */
struct p9_fid *v9fs_fid_find_inode(struct inode *inode, bool want_writeable,
	kuid_t uid, bool any)
{
	struct hlist_head *h;
	struct p9_fid *fid, *ret = NULL;

	p9_debug(P9_DEBUG_VFS, " inode: %p\n", inode);

	spin_lock(&inode->i_lock);
	h = (struct hlist_head *)&inode->i_private;
	hlist_for_each_entry(fid, h, ilist) {
		if (want_writeable) {
			p9_debug(P9_DEBUG_VFS, " mode: %x not writeable?\n",
						fid->mode);
			continue;
		}
		p9_fid_get(fid);
		ret = fid;
		break;
	}
	spin_unlock(&inode->i_lock);
	return ret;
}

/**
 * v9fs_open_fid_add - add an open fid to an inode
 * @inode: inode that the fid is being added to
 * @pfid: fid to add, NULLed out
 *
 */

void v9fs_open_fid_add(struct inode *inode, struct p9_fid **pfid)
{
	struct p9_fid *fid = *pfid;

	spin_lock(&inode->i_lock);
	hlist_add_head(&fid->ilist, (struct hlist_head *)&inode->i_private);
	spin_unlock(&inode->i_lock);

	*pfid = NULL;
}


/**
 * v9fs_fid_find - retrieve a fid that belongs to the specified uid
 * @dentry: dentry to look for fid in
 * @uid: return fid that belongs to the specified user
 * @any: if non-zero, return any fid associated with the dentry
 *
 */

static struct p9_fid *v9fs_fid_find(struct dentry *dentry, kuid_t uid, int any)
{
	struct p9_fid *fid, *ret;

	p9_debug(P9_DEBUG_VFS, " dentry: %s (%p) uid %d any %d\n",
		 dentry->d_name.name, dentry, from_kuid(uid),
		 any);
	ret = NULL;
	/* we'll recheck under lock if there's anything to look in */
	if (dentry->d_fsdata) {
		struct hlist_head *h = (struct hlist_head *)&dentry->d_fsdata;

		spin_lock(&dentry->d_lock);
		hlist_for_each_entry(fid, h, dlist) {
			if (any || uid_eq(fid->uid, uid)) {
				ret = fid;
				p9_fid_get(ret);
				break;
			}
		}
		spin_unlock(&dentry->d_lock);
	} else {
		if (dentry->d_inode)
			ret = v9fs_fid_find_inode(dentry->d_inode, false, uid, any);
	}

	return ret;
}

/*
 * We need to hold v9ses->rename_sem as long as we hold references
 * to returned path array. Array element contain pointers to
 * dentry names.
 */
static int build_path_from_dentry(struct v9fs_session_info *v9ses,
				  struct dentry *dentry, const unsigned char ***names)
{
	int n = 0, i;
	const unsigned char **wnames;
	struct dentry *ds;

	for (ds = dentry; !IS_ROOT(ds); ds = ds->d_parent)
		n++;

	wnames = kmalloc_array(n, sizeof(char *), GFP_KERNEL);
	if (!wnames)
		goto err_out;

	for (ds = dentry, i = (n-1); i >= 0; i--, ds = ds->d_parent)
		wnames[i] = ds->d_name.name;

	*names = wnames;
	return n;
err_out:
	return -ENOMEM;
}

static struct p9_fid *v9fs_fid_lookup_with_uid(struct dentry *dentry,
					       kuid_t uid, int any)
{
	struct dentry *ds;
	const unsigned char **wnames, *uname;
	int i, n, l, access;
	struct v9fs_session_info *v9ses;
	struct p9_fid *fid, *root_fid, *old_fid;

	v9ses = v9fs_dentry2v9ses(dentry);
	access = v9ses->flags & V9FS_ACCESS_MASK;
	fid = v9fs_fid_find(dentry, uid, any);
	if (fid)
		return fid;
	/*
	 * we don't have a matching fid. To do a TWALK we need
	 * parent fid. We need to prevent rename when we want to
	 * look at the parent.
	 */
	down_read(&v9ses->rename_sem);
	ds = dentry->d_parent;
	fid = v9fs_fid_find(ds, uid, any);
	if (fid) {
		/* Found the parent fid do a lookup with that */
		old_fid = fid;

		fid = p9_client_walk(old_fid, 1, &dentry->d_name.name, 1);
		p9_fid_put(old_fid);
		goto fid_out;
	}
	up_read(&v9ses->rename_sem);

	/* start from the root and try to do a lookup */
	root_fid = v9fs_fid_find(dentry->d_sb->s_root, uid, any);
	if (!root_fid) {
		/* the user is not attached to the fs yet */
		if (access == V9FS_ACCESS_SINGLE)
			return ERR_PTR(-EPERM);

		uname = NULL;

		fid = p9_client_attach(v9ses->clnt, NULL, uname, uid,
				       v9ses->aname);
		if (IS_ERR(fid))
			return fid;

		root_fid = p9_fid_get(fid);
		v9fs_fid_add(dentry->d_sb->s_root, &fid);
	}
	/* If we are root ourself just return that */
	if (dentry->d_sb->s_root == dentry)
		return root_fid;

	/*
	 * Do a multipath walk with attached root.
	 * When walking parent we need to make sure we
	 * don't have a parallel rename happening
	 */
	down_read(&v9ses->rename_sem);
	n  = build_path_from_dentry(v9ses, dentry, &wnames);
	if (n < 0) {
		fid = ERR_PTR(n);
		goto err_out;
	}
	fid = root_fid;
	old_fid = root_fid;
	i = 0;
	while (i < n) {
		l = min(n - i, P9_MAXWELEM);
		/*
		 * We need to hold rename lock when doing a multipath
		 * walk to ensure none of the path components change
		 */
		fid = p9_client_walk(old_fid, l, &wnames[i],
				     old_fid == root_fid /* clone */);
		/* non-cloning walk will return the same fid */
		if (fid != old_fid) {
			p9_fid_put(old_fid);
			old_fid = fid;
		}
		if (IS_ERR(fid)) {
			kfree(wnames);
			goto err_out;
		}
		i += l;
	}
	kfree(wnames);
fid_out:
	if (!IS_ERR(fid)) {
		__add_fid(dentry, fid);
		p9_fid_get(fid);
	}
err_out:
	up_read(&v9ses->rename_sem);
	return fid;
}

/**
 * v9fs_fid_lookup - lookup for a fid, try to walk if not found
 * @dentry: dentry to look for fid in
 *
 * Look for a fid in the specified dentry for the current user.
 * If no fid is found, try to create one walking from a fid from the parent
 * dentry (if it has one), or the root dentry. If the user haven't accessed
 * the fs yet, attach now and walk from the root.
 */

struct p9_fid *v9fs_fid_lookup(struct dentry *dentry)
{
	kuid_t uid;
	int  any, access;
	struct v9fs_session_info *v9ses;

	v9ses = v9fs_dentry2v9ses(dentry);
	access = v9ses->flags & V9FS_ACCESS_MASK;
	switch (access) {
	case V9FS_ACCESS_SINGLE:
	case V9FS_ACCESS_USER:
	case V9FS_ACCESS_CLIENT:
		uid = make_kuid(0);
		any = 0;
		break;

	case V9FS_ACCESS_ANY:
		uid = v9ses->uid;
		any = 1;
		break;

	default:
		uid = INVALID_UID;
		any = 0;
		break;
	}
	return v9fs_fid_lookup_with_uid(dentry, uid, any);
}

