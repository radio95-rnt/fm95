#pragma once
#include <stdio.h>
#define debug_printf(fmt, ...) \
	printf("[%s:%d in %s] " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
