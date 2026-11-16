#ifndef NATIVE_SYSTEM_LANGUAGE_HORIZON_H
#define NATIVE_SYSTEM_LANGUAGE_HORIZON_H
#include "../domain/system_language.h"
#include <cstdint>

// Synchronous composition-time SDK lifecycle. Callbacks are nonthrowing and
// serialized; no callback or context is retained after the function returns.
// Result 0 means success. Decode yields the pinned libnx SetLanguage numeric ID.
struct HorizonSystemLanguagePort {
    void *context=nullptr;
    uint32_t (*initialize)(void *)=nullptr;
    uint32_t (*get_language_code)(void *,uint64_t *)=nullptr;
    uint32_t (*decode_language)(void *,uint64_t,int32_t *)=nullptr;
    void (*exit)(void *)=nullptr;
};
enum class HorizonLanguageReadStage { InvalidPort,Initialize,GetLanguageCode,DecodeLanguage,MapLanguage,Complete };
struct HorizonSystemLanguageRead {
    SystemLanguageSnapshot snapshot{};
    HorizonLanguageReadStage stage=HorizonLanguageReadStage::InvalidPort;
    uint32_t native_result=0;
    uint64_t language_code=0;     // valid after a successful get, including decode failure
    int32_t horizon_language=-1; // valid after successful decode, including unsupported IDs
    bool initialize_succeeded=false,exit_called=false;
};
// No fallback setting: missing/failing ports return Unavailable; unknown IDs
// return Unsupported. Exit runs exactly once iff initialization succeeded.
HorizonSystemLanguageRead system_language_horizon_read(const HorizonSystemLanguagePort &);
// SDK-backed overload is defined only by Horizon builds (__SWITCH__).
HorizonSystemLanguageRead system_language_horizon_read();
#endif
