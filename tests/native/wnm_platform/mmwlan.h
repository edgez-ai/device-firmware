#ifndef TEST_MMWLAN_H
#define TEST_MMWLAN_H
#include <stdbool.h>
#define MMWLAN_SUCCESS 0
struct mmwlan_set_wnm_sleep_enabled_args { bool wnm_sleep_enabled, chip_powerdown_enabled; };
#define MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT { false, false }
int mmwlan_set_wnm_sleep_enabled_ext(const struct mmwlan_set_wnm_sleep_enabled_args *args);
#endif
