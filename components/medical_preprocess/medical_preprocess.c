#include "medical_preprocess.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *wrong;
    const char *right;
} s_fix[] = {
    {"感帽", "感冒"},
    {"流敢", "流感"},
    {"头通", "头痛"},
    {"发稍", "发烧"},
    {"腹泄", "腹泻"},
    {"唐尿病", "糖尿病"},
};

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

char *medical_normalize_query(const char *user_text)
{
    if (!user_text) {
        return NULL;
    }
    size_t len = strlen(user_text);
    char *buf = (char *)malloc(len * 3 + 64);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, user_text, len + 1);
    trim_inplace(buf);

    for (size_t i = 0; i < sizeof(s_fix) / sizeof(s_fix[0]); i++) {
        char *p = buf;
        while ((p = strstr(p, s_fix[i].wrong)) != NULL) {
            size_t wl = strlen(s_fix[i].wrong);
            size_t rl = strlen(s_fix[i].right);
            size_t tail_len = strlen(p + wl);
            memmove(p + rl, p + wl, tail_len + 1);
            memcpy(p, s_fix[i].right, rl);
            p += rl;
        }
    }
    return buf;
}

const char *medical_system_prompt(void)
{
    return
        "你是面向基层与家庭场景的健康科普助手，非执业医师，不提供诊断或处方。"
        "回答需简洁、通俗，涉及用药必须提示遵医嘱；急重症请建议立即就医。"
        "回答只使用常用简体中文、英文、数字和常见标点，不使用 emoji、罕见汉字或特殊符号。"
        "若用户描述模糊，可先列出需要澄清的要点。"
        "单次回答不超过 80 字，优先直接给出结论与要点，避免冗长铺垫。";
}
