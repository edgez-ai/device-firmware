#pragma once
#include <stddef.h>
#include <sys/types.h>
typedef ssize_t (*settings_read_cb)(void *, void *, size_t);
extern int (*test_settings_load)(const char *, size_t, settings_read_cb, void *);
#define SETTINGS_STATIC_HANDLER_DEFINE(name, prefix, get, set, commit, export) \
    int (*test_settings_load)(const char *, size_t, settings_read_cb, void *) = set
int settings_subsys_init(void);
int settings_load_subtree(const char *);
int settings_save_one(const char *, const void *, size_t);
