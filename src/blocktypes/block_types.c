#include "blocktypes/block_types.h"

#include <string.h>

#include "blocktypes/builtins.h"

_Static_assert(HAKKAS_BLOCK_TYPES_MAX >= 1, "HAKKAS_BLOCK_TYPES_MAX out of range");

static const BlockTypeDef *types[HAKKAS_BLOCK_TYPES_MAX];
static size_t types_len;

/* The index of the type called name, or types_len. */
static size_t find(const char *name)
{
	size_t i;
	for (i = 0; i < types_len; i++) {
		if (strcmp(types[i]->name, name) == 0)
			break;
	}
	return i;
}

enum block_types_err block_types_register(const BlockTypeDef *def)
{
	if (def == NULL || def->name == NULL || def->name[0] == '\0')
		return BLOCK_TYPES_ERR_INVALID;
	if (find(def->name) < types_len)
		return BLOCK_TYPES_ERR_EXISTS;
	if (types_len == HAKKAS_BLOCK_TYPES_MAX)
		return BLOCK_TYPES_ERR_FULL;
	types[types_len++] = def;
	return BLOCK_TYPES_OK;
}

bool block_types_unregister(const char *name)
{
	if (name == NULL)
		return false;
	size_t i = find(name);
	if (i == types_len)
		return false;
	memmove(&types[i], &types[i + 1], (types_len - i - 1) * sizeof(types[0]));
	types[--types_len] = NULL;
	return true;
}

const BlockTypeDef *block_types_get(const char *name)
{
	if (name == NULL)
		return NULL;
	size_t i = find(name);
	return i < types_len ? types[i] : NULL;
}

size_t block_types_count(void)
{
	return types_len;
}

const BlockTypeDef *block_types_at(size_t i)
{
	return i < types_len ? types[i] : NULL;
}

void block_types_reset(void)
{
	memset(types, 0, sizeof(types));
	types_len = 0;
}

static enum block_types_err register_all(const BlockTypeDef *defs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		enum block_types_err err = block_types_register(&defs[i]);
		if (err != BLOCK_TYPES_OK)
			return err;
	}
	return BLOCK_TYPES_OK;
}

enum block_types_err block_types_register_builtins(void)
{
	enum block_types_err err = register_all(math_block_types, math_block_types_count);
	if (err == BLOCK_TYPES_OK)
		err = register_all(process_block_types, process_block_types_count);
	return err;
}
