#ifndef NATIVE_SYSTEM_LANGUAGE_H
#define NATIVE_SYSTEM_LANGUAGE_H
#include <cstdint>

// PS3 language vocabulary; guest-SDK compatibility policy belongs to its caller.
enum class SystemLanguage : uint32_t {
    Japanese=0,EnglishUS=1,French=2,Spanish=3,German=4,Italian=5,Dutch=6,
    PortuguesePT=7,Russian=8,Korean=9,ChineseTraditional=10,ChineseSimplified=11,
    Finnish=12,Swedish=13,Danish=14,Norwegian=15,Polish=16,PortugueseBR=17,
    EnglishGB=18,Turkish=19
};
enum class SystemLanguageStatus { Unavailable,Available,Unsupported };
// Composition samples once, then main/children retain independent value copies.
// language is meaningful only when status is Available; no default is admitted.
struct SystemLanguageSnapshot {
    SystemLanguageStatus status=SystemLanguageStatus::Unavailable;
    SystemLanguage language=SystemLanguage::Japanese;
};
#endif
