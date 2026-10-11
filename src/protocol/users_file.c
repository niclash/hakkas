/*
 * The users file through the PAL (02 §2.2, 05 §2.3). See protocol/users.h.
 */
#include <stdlib.h>
#include <string.h>

#include "protocol/users.h"
#include "system/log.h"

enum users_status users_load(UserStore *s, const char *path)
{
	users_init(s);
	struct pal_stat info;
	enum pal_err err = pal_storage_stat(path, &info);
	if (err == PAL_ERR_NOT_FOUND)
		return USERS_ERR_NOT_FOUND;
	if (err == PAL_OK && info.is_dir)
		err = PAL_ERR_INVALID;
	if (err != PAL_OK) {
		log_error("cannot read %s: %s", path, pal_err_str(err));
		return USERS_ERR_STORAGE;
	}
	if (info.size > USERS_FILE_MAX) {
		log_error("%s: more than %lu bytes", path, (unsigned long)USERS_FILE_MAX);
		return USERS_ERR_MALFORMED;
	}
	size_t cap = (size_t)info.size;
	char *buf = malloc(cap + 1);
	if (buf == NULL)
		return USERS_ERR_NO_MEMORY;
	size_t len = 0;
	err = pal_storage_read(path, buf, cap, &len);
	enum users_status st;
	if (err == PAL_ERR_TOO_BIG) {
		log_error("%s: more than %lu bytes", path, (unsigned long)USERS_FILE_MAX);
		st = USERS_ERR_MALFORMED; /* it grew since the stat */
	} else if (err != PAL_OK) {
		log_error("cannot read %s: %s", path, pal_err_str(err));
		st = USERS_ERR_STORAGE;
	} else {
		st = users_parse(s, buf, len, path);
	}
	crypto_zeroize(buf, cap + 1);
	free(buf);
	return st;
}

enum users_status users_load_startup(UserStore *s, const char *path)
{
	enum users_status st = users_load(s, path);
	if (st == USERS_ERR_NOT_FOUND)
		log_warn("%s not found: no users provisioned, every login is refused", path);
	else if (st == USERS_OK && s->count == 0)
		log_warn("%s holds no users: every login is refused", path);
	else if (st != USERS_OK)
		log_error("%s: %s: every login is refused", path, users_status_str(st));
	else
		log_info("%s: %zu users", path, s->count);
	return st;
}

enum users_status users_loader(void *path, UserStore *s)
{
	return users_load(s, path);
}

enum users_status users_save(const UserStore *s, const char *path)
{
	size_t len = 0;
	char *text = users_serialize(s, &len);
	if (text == NULL)
		return USERS_ERR_NO_MEMORY;
	enum pal_err err = pal_storage_write(path, text, len);
	crypto_zeroize(text, len);
	free(text);
	if (err != PAL_OK) {
		log_error("cannot write %s: %s", path, pal_err_str(err));
		return USERS_ERR_STORAGE;
	}
	return USERS_OK;
}
