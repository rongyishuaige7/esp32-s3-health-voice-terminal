#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 口语归一 + 简单同音/错别字替换；输出堆缓冲区，调用方 free。
 */
char *medical_normalize_query(const char *user_text);

/** 供 DeepSeek system 角色的固定约束（基层咨询、非诊断） */
const char *medical_system_prompt(void);

#ifdef __cplusplus
}
#endif
