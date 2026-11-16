#include "system_language_horizon.h"

namespace {
bool map_language(int32_t id,SystemLanguage &language) {
    switch(id) {
    case 0: language=SystemLanguage::Japanese; break;
    case 1: language=SystemLanguage::EnglishUS; break;
    case 2: case 13: language=SystemLanguage::French; break;
    case 3: language=SystemLanguage::German; break;
    case 4: language=SystemLanguage::Italian; break;
    case 5: case 14: language=SystemLanguage::Spanish; break;
    case 6: case 15: language=SystemLanguage::ChineseSimplified; break;
    case 7: language=SystemLanguage::Korean; break;
    case 8: language=SystemLanguage::Dutch; break;
    case 9: language=SystemLanguage::PortuguesePT; break;
    case 10: language=SystemLanguage::Russian; break;
    case 11: case 16: language=SystemLanguage::ChineseTraditional; break;
    case 12: language=SystemLanguage::EnglishGB; break;
    case 17: language=SystemLanguage::PortugueseBR; break;
    default: return false;
    }
    return true;
}
}

HorizonSystemLanguageRead system_language_horizon_read(const HorizonSystemLanguagePort &source) {
    const auto port=source;
    HorizonSystemLanguageRead result{};
    if(!port.initialize || !port.get_language_code || !port.decode_language || !port.exit)
        return result;
    result.stage=HorizonLanguageReadStage::Initialize;
    result.native_result=port.initialize(port.context);
    if(result.native_result) return result;
    result.initialize_succeeded=true;
    const auto finish=[&]() {
        port.exit(port.context);
        result.exit_called=true;
    };
    uint64_t code=0;
    result.stage=HorizonLanguageReadStage::GetLanguageCode;
    result.native_result=port.get_language_code(port.context,&code);
    if(result.native_result) { finish(); return result; }
    result.language_code=code;
    int32_t id=-1;
    result.stage=HorizonLanguageReadStage::DecodeLanguage;
    result.native_result=port.decode_language(port.context,code,&id);
    if(result.native_result) { finish(); return result; }
    result.horizon_language=id;
    result.stage=HorizonLanguageReadStage::MapLanguage;
    if(map_language(id,result.snapshot.language)) {
        result.snapshot.status=SystemLanguageStatus::Available;
        result.stage=HorizonLanguageReadStage::Complete;
    } else {
        result.snapshot.status=SystemLanguageStatus::Unsupported;
    }
    finish();
    return result;
}

#ifdef __SWITCH__
#include <switch.h>
namespace {
static_assert(SetLanguage_JA==0 && SetLanguage_ENUS==1 && SetLanguage_FR==2 &&
    SetLanguage_DE==3 && SetLanguage_IT==4 && SetLanguage_ES==5 && SetLanguage_ZHCN==6 &&
    SetLanguage_KO==7 && SetLanguage_NL==8 && SetLanguage_PT==9 && SetLanguage_RU==10 &&
    SetLanguage_ZHTW==11 && SetLanguage_ENGB==12 && SetLanguage_FRCA==13 &&
    SetLanguage_ES419==14 && SetLanguage_ZHHANS==15 && SetLanguage_ZHHANT==16 &&
    SetLanguage_PTBR==17,"Horizon language mapping must match the pinned SDK");
uint32_t initialize_sdk(void *) { return setInitialize(); }
uint32_t get_sdk(void *,uint64_t *code) { return setGetSystemLanguage(code); }
uint32_t decode_sdk(void *,uint64_t code,int32_t *id) {
    SetLanguage language{};
    const Result result=setMakeLanguage(code,&language);
    if(R_SUCCEEDED(result)) *id=static_cast<int32_t>(language);
    return result;
}
void exit_sdk(void *) { setExit(); }
}
HorizonSystemLanguageRead system_language_horizon_read() {
    const HorizonSystemLanguagePort port{nullptr,initialize_sdk,get_sdk,decode_sdk,exit_sdk};
    return system_language_horizon_read(port);
}
#endif
