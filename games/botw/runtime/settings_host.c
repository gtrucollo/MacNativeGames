/* The "set" service (nn::settings): language and region, answered from the same choice as nn::oe::GetDesiredLanguage.
   Breath of the Wild's SDK asks the service for these (TOTK's reads them through nn::oe and never opens it). */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>

uint64_t runtime_desired_language(void);   /* services.c */

/* nn::settings::Language order */
static const char *const language_codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                              "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NLANG (sizeof language_codes / sizeof *language_codes)

static uint64_t code_value(const char *name) { uint64_t v = 0; memcpy(&v, name, strlen(name) < 8 ? strlen(name) : 8); return v; }

static int32_t region_of(uint64_t code) {   /* 0 Japan, 1 USA, 2 Europe, 4 China, 5 Korea, 6 Taiwan */
    if (code == code_value("ja")) return 0;
    if (code == code_value("en-US")) return 1;
    if (code == code_value("zh-CN")) return 4;
    if (code == code_value("ko")) return 5;
    if (code == code_value("zh-TW")) return 6;
    return 2;
}

static void write_codes(const IpcRequest *rq, IpcResponse *rs, int32_t count) {
    uint64_t codes[NLANG];
    for (unsigned i = 0; i < NLANG; i++) codes[i] = code_value(language_codes[i]);
    uint64_t room = rq->nb && rq->b[0].size ? rq->b[0].size : rq->nc ? rq->c[0].size : 0;
    uint64_t addr = rq->nb && rq->b[0].size ? rq->b[0].addr : rq->nc ? rq->c[0].addr : 0;
    uint64_t bytes = (uint64_t)count * 8 < room ? (uint64_t)count * 8 : room;
    if (bytes) memcpy(ipc_guest(addr, bytes), codes, bytes);
    if (!(rq->nb && rq->b[0].size) && rq->nc && bytes) rs->xout[rs->nxout++] = (IpcBuffer){addr, bytes, 0};
}

static void handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t v = runtime_desired_language(); IPC_OUT(rs, v); return; }                              /* GetLanguageCode */
    case 1: case 5: { int32_t n = NLANG; write_codes(rq, rs, n); IPC_OUT(rs, n); return; }                    /* GetAvailableLanguageCodes(2) */
    case 2: { int32_t language = IPC_ARG(rq, int32_t, 0);                                                     /* MakeLanguageCode */
              uint64_t v = language >= 0 && (unsigned)language < NLANG ? code_value(language_codes[language]) : code_value("en-US");
              IPC_OUT(rs, v); return; }
    case 3: case 6: { int32_t n = NLANG; IPC_OUT(rs, n); return; }                                           /* GetAvailableLanguageCodeCount(2) */
    case 4: { int32_t v = region_of(runtime_desired_language()); IPC_OUT(rs, v); return; }                    /* GetRegionCode */
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_settings(void) { runtime_ipc_register("set", handler, NULL, NULL); }
