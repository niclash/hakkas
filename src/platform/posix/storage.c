/*
 * POSIX PAL storage (01 §0.1, 05 §2.3): plain files in a directory tree,
 * written atomically through <path>.tmp, fsync and rename; streaming reads
 * through a file descriptor (pal_file_*).
 *
 * Every system call that can be interrupted is retried on EINTR; reads and
 * writes loop over partial transfers. Files are created with mode 0600: the
 * same procedure writes the credential store (02 §2.2).
 */
#include "platform/pal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "system/log.h"

#define FILE_MODE 0600
#define DIR_MODE  0755

static enum pal_err from_errno(int err)
{
	switch (err) {
	case ENOENT:
	case ENOTDIR:
		return PAL_ERR_NOT_FOUND;
	case EEXIST:
	case ENOTEMPTY:
		return PAL_ERR_EXISTS;
	case ENAMETOOLONG:
		return PAL_ERR_NAME_TOO_LONG;
	case ENOSPC:
	case EDQUOT:
	case EFBIG:
		return PAL_ERR_NO_SPACE;
	case EACCES:
	case EPERM:
	case EROFS:
		return PAL_ERR_DENIED;
	case EISDIR:
	case EINVAL:
	case EXDEV:
		return PAL_ERR_INVALID;
	case EMFILE:
	case ENFILE:
	case ENOMEM:
		return PAL_ERR_UNAVAILABLE;
	default:
		return PAL_ERR_IO;
	}
}

/* Validates a path argument: non-NULL, non-empty, and with extra more bytes
 * (plus the NUL) fitting PAL_PATH_MAX. */
static enum pal_err check_path(const char *path, size_t extra)
{
	if (path == NULL || path[0] == '\0')
		return PAL_ERR_INVALID;
	if (strlen(path) + extra >= PAL_PATH_MAX)
		return PAL_ERR_NAME_TOO_LONG;
	return PAL_OK;
}

static int open_retry(const char *path, int flags, mode_t mode)
{
	int fd;
	do {
		fd = open(path, flags | O_CLOEXEC, mode);
	} while (fd < 0 && errno == EINTR);
	return fd;
}

/* close() is not retried on EINTR: on Linux the descriptor is released anyway. */
static void close_quietly(int fd)
{
	int saved = errno;
	close(fd);
	errno = saved;
}

static int fsync_retry(int fd)
{
	int rc;
	do {
		rc = fsync(fd);
	} while (rc != 0 && errno == EINTR);
	return rc;
}

/* Writes the directory part of path (everything before the last '/'; "." if
 * there is none, "/" for a file in the root) into dir, PAL_PATH_MAX bytes. */
static void dir_of(const char *path, char dir[PAL_PATH_MAX])
{
	const char *slash = strrchr(path, '/');
	if (slash == NULL) {
		strcpy(dir, ".");
	} else if (slash == path) {
		strcpy(dir, "/");
	} else {
		size_t n = (size_t)(slash - path);
		memcpy(dir, path, n);
		dir[n] = '\0';
	}
}

/* Flushes the directory containing path, so a rename or unlink in it is
 * durable. Filesystems that cannot sync a directory (EINVAL) are tolerated. */
static enum pal_err sync_dir_of(const char *path)
{
	char dir[PAL_PATH_MAX];
	dir_of(path, dir);
	int fd = open_retry(dir, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0)
		return from_errno(errno);
	enum pal_err err = PAL_OK;
	if (fsync_retry(fd) != 0 && errno != EINVAL)
		err = from_errno(errno);
	close_quietly(fd);
	return err;
}

enum pal_err pal_storage_stat(const char *path, struct pal_stat *info)
{
	enum pal_err err = check_path(path, 0);
	if (err != PAL_OK)
		return err;
	struct stat st;
	if (stat(path, &st) != 0)
		return from_errno(errno);
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		return PAL_ERR_INVALID;
	if (info != NULL) {
		info->is_dir = S_ISDIR(st.st_mode);
		info->size = info->is_dir ? 0 : (uint64_t)st.st_size;
	}
	return PAL_OK;
}

bool pal_storage_exists(const char *path)
{
	return pal_storage_stat(path, NULL) == PAL_OK;
}

/* Reads up to cap bytes; returns the count, or -1 with errno set. */
static ssize_t read_full(int fd, unsigned char *buf, size_t cap)
{
	size_t total = 0;
	while (total < cap) {
		ssize_t n = read(fd, buf + total, cap - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	return (ssize_t)total;
}

enum pal_err pal_storage_read(const char *path, void *buf, size_t cap, size_t *len)
{
	enum pal_err err = check_path(path, 0);
	if (err != PAL_OK)
		return err;
	if (len == NULL || (buf == NULL && cap > 0))
		return PAL_ERR_INVALID;

	int fd = open_retry(path, O_RDONLY, 0);
	if (fd < 0)
		return from_errno(errno);

	struct stat st;
	if (fstat(fd, &st) != 0) {
		err = from_errno(errno);
		goto out;
	}
	if (!S_ISREG(st.st_mode)) {
		err = PAL_ERR_INVALID;
		goto out;
	}
	if ((uint64_t)st.st_size > cap) {
		uint64_t size = (uint64_t)st.st_size;
		*len = size < (uint64_t)SIZE_MAX ? (size_t)size : SIZE_MAX;
		err = PAL_ERR_TOO_BIG;
		goto out;
	}

	ssize_t n = read_full(fd, buf, cap);
	if (n < 0) {
		err = from_errno(errno);
		goto out;
	}
	if ((size_t)n == cap) {
		/* The file may have grown since fstat: content must fit cap. */
		unsigned char extra;
		ssize_t m = read_full(fd, &extra, 1);
		if (m < 0) {
			err = from_errno(errno);
			goto out;
		}
		if (m > 0) {
			*len = cap < SIZE_MAX ? cap + 1 : SIZE_MAX;
			err = PAL_ERR_TOO_BIG;
			goto out;
		}
	}
	*len = (size_t)n;
out:
	close_quietly(fd);
	return err;
}

static bool write_full(int fd, const unsigned char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		data += n;
		len -= (size_t)n;
	}
	return true;
}

enum pal_err pal_storage_write(const char *path, const void *data, size_t len)
{
	enum pal_err err = check_path(path, strlen(PAL_TMP_SUFFIX));
	if (err != PAL_OK)
		return err;
	if (data == NULL && len > 0)
		return PAL_ERR_INVALID;

	/* 05 §2.3 step 1: the complete content goes to <path>.tmp. */
	char tmp[PAL_PATH_MAX];
	strcpy(tmp, path);
	strcat(tmp, PAL_TMP_SUFFIX);

	int fd = open_retry(tmp, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
	if (fd < 0)
		return from_errno(errno);

	/* Step 2: flush to stable storage, then close. */
	bool ok = write_full(fd, data, len) && fsync_retry(fd) == 0;
	if (!ok) {
		err = from_errno(errno);
		close_quietly(fd);
		goto fail;
	}
	if (close(fd) != 0 && errno != EINTR) {
		err = from_errno(errno);
		goto fail;
	}

	/* Step 3: rename over path, then make the rename durable. */
	if (rename(tmp, path) != 0) {
		err = from_errno(errno);
		goto fail;
	}
	err = sync_dir_of(path);
	if (err != PAL_OK)
		log_warn("%s written, but its directory could not be synced: %s", path,
			 pal_err_str(err));
	return err;

fail:
	unlink(tmp);
	return err;
}

enum pal_err pal_storage_rename(const char *from, const char *to)
{
	enum pal_err err = check_path(from, 0);
	if (err == PAL_OK)
		err = check_path(to, 0);
	if (err != PAL_OK)
		return err;
	struct stat st;
	if (stat(from, &st) != 0)
		return from_errno(errno);
	if (!S_ISREG(st.st_mode))
		return PAL_ERR_INVALID;
	if (rename(from, to) != 0)
		return from_errno(errno);

	char from_dir[PAL_PATH_MAX], to_dir[PAL_PATH_MAX];
	dir_of(from, from_dir);
	dir_of(to, to_dir);
	err = sync_dir_of(to);
	if (err == PAL_OK && strcmp(from_dir, to_dir) != 0)
		err = sync_dir_of(from);
	return err;
}

enum pal_err pal_storage_remove(const char *path)
{
	enum pal_err err = check_path(path, 0);
	if (err != PAL_OK)
		return err;
	if (unlink(path) != 0) {
		/* Linux reports EISDIR, POSIX allows EPERM, for a directory. */
		int saved = errno;
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			return PAL_ERR_INVALID;
		return from_errno(saved);
	}
	return sync_dir_of(path);
}

enum pal_err pal_storage_mkdir(const char *path)
{
	enum pal_err err = check_path(path, 0);
	if (err != PAL_OK)
		return err;
	if (mkdir(path, DIR_MODE) == 0)
		return sync_dir_of(path);
	int saved = errno;
	if (saved == EEXIST) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			return PAL_OK;
		return PAL_ERR_EXISTS;
	}
	return from_errno(saved);
}

/* Opens dir for listing. */
static enum pal_err open_dir(const char *dir, DIR **out)
{
	enum pal_err err = check_path(dir, 0);
	if (err != PAL_OK)
		return err;
	int fd = open_retry(dir, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0)
		return from_errno(errno);
	*out = fdopendir(fd);
	if (*out == NULL) {
		err = from_errno(errno);
		close_quietly(fd);
		return err;
	}
	return PAL_OK;
}

/* The next entry of d other than "." and "..", with its kind; regular files
 * and directories only (symbolic links are followed, other kinds skipped).
 * Returns NULL at the end, or on error with *err set. */
static struct dirent *next_entry(DIR *d, bool *is_dir, enum pal_err *err)
{
	for (;;) {
		errno = 0;
		struct dirent *e = readdir(d);
		if (e == NULL) {
			*err = errno != 0 ? from_errno(errno) : PAL_OK;
			return NULL;
		}
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (strlen(e->d_name) >= PAL_NAME_MAX)
			continue;
		if (e->d_type == DT_REG || e->d_type == DT_DIR) {
			*is_dir = e->d_type == DT_DIR;
			return e;
		}
		/* DT_UNKNOWN (some filesystems) or a link: ask the inode. */
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, 0) != 0)
			continue; /* vanished or a dangling link */
		if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
			*is_dir = S_ISDIR(st.st_mode);
			return e;
		}
	}
}

enum pal_err pal_storage_list(const char *dir, pal_storage_list_fn fn, void *ctx)
{
	if (fn == NULL)
		return PAL_ERR_INVALID;
	DIR *d;
	enum pal_err err = open_dir(dir, &d);
	if (err != PAL_OK)
		return err;
	struct dirent *e;
	bool is_dir;
	while ((e = next_entry(d, &is_dir, &err)) != NULL) {
		if (!fn(ctx, e->d_name, is_dir))
			break;
	}
	closedir(d);
	return err;
}

static bool has_tmp_suffix(const char *name)
{
	size_t n = strlen(name), s = strlen(PAL_TMP_SUFFIX);
	return n >= s && strcmp(name + n - s, PAL_TMP_SUFFIX) == 0;
}

enum pal_err pal_storage_remove_tmp(const char *dir, size_t *removed)
{
	size_t count = 0;
	if (removed != NULL)
		*removed = 0;
	DIR *d;
	enum pal_err err = open_dir(dir, &d);
	if (err != PAL_OK)
		return err;
	/* Unlinking the entry just read is safe while iterating: POSIX leaves
	 * only whether that entry is returned again unspecified, and a second
	 * unlinkat of it fails with ENOENT, which is skipped. */
	struct dirent *e;
	bool is_dir;
	while ((e = next_entry(d, &is_dir, &err)) != NULL) {
		if (is_dir || !has_tmp_suffix(e->d_name))
			continue;
		if (unlinkat(dirfd(d), e->d_name, 0) != 0) {
			if (errno == ENOENT)
				continue;
			err = from_errno(errno);
			log_warn("cannot remove leftover %s/%s: %s", dir, e->d_name,
				 pal_err_str(err));
			break;
		}
		log_info("removed leftover %s/%s from an interrupted write", dir, e->d_name);
		count++;
	}
	if (count > 0 && fsync_retry(dirfd(d)) != 0 && errno != EINVAL && err == PAL_OK)
		err = from_errno(errno);
	closedir(d);
	if (removed != NULL)
		*removed = count;
	return err;
}

/* ---- Streaming reads ---------------------------------------------------------- */

enum pal_err pal_file_open(const char *path, pal_file *out, uint64_t *size)
{
	*out = PAL_FILE_NONE;
	enum pal_err err = check_path(path, 0);
	if (err != PAL_OK)
		return err;
	/* O_NONBLOCK: opening a FIFO must not wait for a writer; it does not
	 * affect reading a regular file. */
	int fd = open_retry(path, O_RDONLY | O_NONBLOCK, 0);
	if (fd < 0)
		return from_errno(errno);
	struct stat st;
	if (fstat(fd, &st) != 0) {
		err = from_errno(errno);
		close_quietly(fd);
		return err;
	}
	if (!S_ISREG(st.st_mode)) {
		close_quietly(fd);
		return PAL_ERR_INVALID;
	}
	if (size != NULL)
		*size = (uint64_t)st.st_size;
	*out = fd;
	return PAL_OK;
}

enum pal_err pal_file_read(pal_file f, void *buf, size_t cap, size_t *len)
{
	*len = 0;
	if (f < 0 || buf == NULL || cap == 0)
		return PAL_ERR_INVALID;
	ssize_t n = read_full(f, buf, cap);
	if (n < 0)
		return from_errno(errno);
	*len = (size_t)n;
	return PAL_OK;
}

void pal_file_close(pal_file f)
{
	if (f >= 0)
		close_quietly(f);
}
