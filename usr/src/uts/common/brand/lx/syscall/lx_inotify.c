/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the License.
 */

/* Kernel-backed Linux inotify calls for restricted lx processes. */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/inotify.h>

#define	LX_INOTIFY_READDIR_SIZE	(16 * 1024)

extern int open(char *, int, int);

static int
lx_inotify_ioctl(file_t *fp, int cmd, intptr_t arg, int *rvp)
{
	return (VOP_IOCTL(fp->f_vnode, cmd, arg, fp->f_flag | FKIOCTL,
	    fp->f_cred, rvp, NULL));
}

static int
lx_inotify_activate(file_t *fp, int wd)
{
	int rv;

	return (lx_inotify_ioctl(fp, INOTIFYIOC_ACTIVATE, wd, &rv));
}

static void
lx_inotify_remove(file_t *fp, int wd)
{
	int rv;

	(void) lx_inotify_ioctl(fp, INOTIFYIOC_RM_WATCH, wd, &rv);
}

static int
lx_inotify_add_children(file_t *ifp, int dirfd, file_t *dfp)
{
	char *buf;
	offset_t offset = 0;
	int error = 0, eof = 0;

	buf = kmem_alloc(LX_INOTIFY_READDIR_SIZE, KM_SLEEP);
	while (!eof) {
		struct iovec iov;
		struct uio uio;
		size_t used, pos;

		iov.iov_base = buf;
		iov.iov_len = LX_INOTIFY_READDIR_SIZE;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_loffset = offset;
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_resid = LX_INOTIFY_READDIR_SIZE;
		uio.uio_fmode = 0;
		uio.uio_extflg = UIO_COPY_CACHED;
		(void) VOP_RWLOCK(dfp->f_vnode, V_WRITELOCK_FALSE, NULL);
		error = VOP_READDIR(dfp->f_vnode, &uio, dfp->f_cred, &eof,
		    NULL, 0);
		VOP_RWUNLOCK(dfp->f_vnode, V_WRITELOCK_FALSE, NULL);
		if (error != 0)
			break;
		used = LX_INOTIFY_READDIR_SIZE - uio.uio_resid;
		if (used == 0)
			break;
		offset = uio.uio_loffset;
		for (pos = 0; pos < used; ) {
			struct dirent64 *dp = (struct dirent64 *)(buf + pos);
			inotify_addchild_t child;
			int rv, e;

			if (dp->d_reclen == 0 || dp->d_reclen > used - pos) {
				error = EIO;
				break;
			}
			pos += dp->d_reclen;
			if (strcmp(dp->d_name, ".") == 0 ||
			    strcmp(dp->d_name, "..") == 0)
				continue;
			bzero(&child, sizeof (child));
			child.inac_name = dp->d_name;
			child.inac_fd = dirfd;
			e = lx_inotify_ioctl(ifp, INOTIFYIOC_ADD_CHILD,
			    (intptr_t)&child, &rv);
			if (e == ENXIO || e == EFAULT || e == EBADF) {
				error = e;
				break;
			}
		}
		if (error != 0)
			break;
	}
	kmem_free(buf, LX_INOTIFY_READDIR_SIZE);
	return (error);
}

long
lx_inotify_init1(int flags)
{
	file_t *fp = NULL;
	vnode_t *vp = NULL;
	int error, fd;
	int fmode = FREAD | FWRITE;

	if ((flags & ~(IN_NONBLOCK | IN_CLOEXEC)) != 0)
		return (set_errno(EINVAL));
	if ((flags & IN_NONBLOCK) != 0)
		fmode |= FNONBLOCK;
	if ((error = falloc(NULL, fmode, &fp, &fd)) != 0)
		return (set_errno(error));
	if ((error = ldi_vp_from_name("/dev/inotify", &vp)) != 0)
		goto fail;
	if ((error = VOP_OPEN(&vp, fmode | FKLYR, CRED(), NULL)) != 0) {
		VN_RELE(vp);
		vp = NULL;
		goto fail;
	}
	fp->f_vnode = vp;
	mutex_exit(&fp->f_tlock);
	setf(fd, fp);
	if ((flags & IN_CLOEXEC) != 0)
		f_setfd_or(fd, FD_CLOEXEC);
	return (fd);

fail:
	setf(fd, NULL);
	unfalloc(fp);
	if (vp != NULL) {
		(void) VOP_CLOSE(vp, fmode, 0, 0, CRED(), NULL);
		VN_RELE(vp);
	}
	return (set_errno(error));
}

long
lx_inotify_init(void)
{
	return (lx_inotify_init1(0));
}

long
lx_inotify_add_watch(int fd, char *pathname, uint32_t mask)
{
	inotify_addwatch_t add;
	file_t *ifp, *dfp;
	int dirfd, error, rv = -1, oflags = O_RDONLY;
	boolean_t isdir;

	if ((ifp = getf(fd)) == NULL)
		return (set_errno(EBADF));
	if ((mask & IN_DONT_FOLLOW) != 0)
		oflags |= O_NOFOLLOW;
	ttolwp(curthread)->lwp_errno = 0;
	dirfd = open(pathname, oflags, 0);
	error = ttolwp(curthread)->lwp_errno;
	if (error != 0) {
		releasef(fd);
		return (set_errno(error));
	}
	if ((dfp = getf(dirfd)) == NULL) {
		(void) closeandsetf(dirfd, NULL);
		releasef(fd);
		return (set_errno(EBADF));
	}
	isdir = dfp->f_vnode->v_type == VDIR;
	if ((mask & IN_ONLYDIR) != 0 && !isdir) {
		releasef(dirfd);
		(void) closeandsetf(dirfd, NULL);
		releasef(fd);
		return (set_errno(ENOTDIR));
	}
	bzero(&add, sizeof (add));
	add.inaw_fd = dirfd;
	add.inaw_mask = mask;
	error = lx_inotify_ioctl(ifp, INOTIFYIOC_ADD_WATCH,
	    (intptr_t)&add, &rv);
	if (error == 0 && isdir && (mask & IN_CHILD_EVENTS) != 0)
		error = lx_inotify_add_children(ifp, dirfd, dfp);
	releasef(dirfd);
	(void) closeandsetf(dirfd, NULL);
	if (error == 0)
		error = lx_inotify_activate(ifp, rv);
	if (error != 0 && rv > 0)
		lx_inotify_remove(ifp, rv);
	releasef(fd);
	if (error != 0)
		return (set_errno(error));
	return (rv);
}

long
lx_inotify_rm_watch(int fd, int wd)
{
	file_t *fp;
	int error, rv;

	if ((fp = getf(fd)) == NULL)
		return (set_errno(EBADF));
	error = lx_inotify_ioctl(fp, INOTIFYIOC_RM_WATCH, wd, &rv);
	releasef(fd);
	if (error != 0)
		return (set_errno(error));
	return (0);
}
