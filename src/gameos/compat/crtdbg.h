#pragma once
// PORT: Debug CRT diagnostics are unavailable in the browser build.
#define _NORMAL_BLOCK 0
#define _CLIENT_BLOCK 0
#define _CrtSetDbgFlag(value) (value)
#define _CrtSetAllocHook(value) ((void)0)
