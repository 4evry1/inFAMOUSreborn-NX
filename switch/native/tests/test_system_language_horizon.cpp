#include "../adapters/system_language_horizon.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

static_assert(std::is_trivially_copyable<SystemLanguageSnapshot>::value);
struct Fixture {
    char calls[5]{};
    unsigned used=0;
    uint32_t init_result=0,get_result=0,decode_result=0;
    uint64_t code=0x1122334455667788ull;
    int32_t language=1;
    HorizonSystemLanguagePort *mutate=nullptr;
    void record(char c) { assert(used<4); calls[used++]=c; }
};
static uint32_t initialize(void *p) {
    auto &f=*static_cast<Fixture *>(p); f.record('I');
    if(f.mutate) *f.mutate={};
    return f.init_result;
}
static uint32_t get_code(void *p,uint64_t *code) {
    auto &f=*static_cast<Fixture *>(p); f.record('G'); *code=f.code; return f.get_result;
}
static uint32_t decode(void *p,uint64_t code,int32_t *language) {
    auto &f=*static_cast<Fixture *>(p); f.record('D'); assert(code==f.code);
    *language=f.language; return f.decode_result;
}
static void finish(void *p) { static_cast<Fixture *>(p)->record('X'); }
static HorizonSystemLanguagePort port(Fixture &f) { return {&f,initialize,get_code,decode,finish}; }
static void mappings() {
    const uint32_t expected[]={0,1,2,4,5,3,11,9,6,7,8,10,18,2,3,11,10,17};
    for(int32_t id=0;id<18;++id) {
        Fixture f; f.language=id; auto r=system_language_horizon_read(port(f));
        assert(std::strcmp(f.calls,"IGDX")==0);
        assert(r.snapshot.status==SystemLanguageStatus::Available);
        assert(static_cast<uint32_t>(r.snapshot.language)==expected[id]);
        assert(r.stage==HorizonLanguageReadStage::Complete && r.native_result==0);
        assert(r.language_code==f.code && r.horizon_language==id);
        assert(r.initialize_succeeded && r.exit_called);
    }
}
static void failures_and_unsupported() {
    for(unsigned stage=0;stage<3;++stage) {
        Fixture f; const uint32_t error=0xa0001234u+stage;
        if(stage==0) f.init_result=error;
        if(stage==1) f.get_result=error;
        if(stage==2) f.decode_result=error;
        const auto r=system_language_horizon_read(port(f));
        assert(r.snapshot.status==SystemLanguageStatus::Unavailable && r.native_result==error);
        const auto expected=stage==0?HorizonLanguageReadStage::Initialize:
            stage==1?HorizonLanguageReadStage::GetLanguageCode:HorizonLanguageReadStage::DecodeLanguage;
        assert(r.stage==expected);
        assert(std::strcmp(f.calls,stage==0?"I":stage==1?"IGX":"IGDX")==0);
        assert(r.initialize_succeeded==(stage!=0) && r.exit_called==(stage!=0));
        assert(r.language_code==(stage==2?f.code:0) && r.horizon_language==-1);
    }
    for(int32_t id:{-1,18,19,99,INT_MIN,INT_MAX}) {
        Fixture f; f.language=id; const auto r=system_language_horizon_read(port(f));
        assert(r.snapshot.status==SystemLanguageStatus::Unsupported);
        assert(r.stage==HorizonLanguageReadStage::MapLanguage && r.native_result==0);
        assert(r.horizon_language==id && r.language_code==f.code);
        assert(r.initialize_succeeded && r.exit_called && std::strcmp(f.calls,"IGDX")==0);
    }
}
static void invalid_ports_and_capture() {
    for(unsigned missing=0;missing<4;++missing) {
        Fixture f; auto p=port(f);
        if(missing==0) p.initialize=nullptr;
        if(missing==1) p.get_language_code=nullptr;
        if(missing==2) p.decode_language=nullptr;
        if(missing==3) p.exit=nullptr;
        auto r=system_language_horizon_read(p);
        assert(r.stage==HorizonLanguageReadStage::InvalidPort);
        assert(r.snapshot.status==SystemLanguageStatus::Unavailable);
        assert(f.used==0 && !r.initialize_succeeded && !r.exit_called && !r.native_result);
    }
    Fixture f; auto p=port(f); f.mutate=&p;
    auto r=system_language_horizon_read(p);
    assert(!p.initialize && r.snapshot.status==SystemLanguageStatus::Available);
    assert(std::strcmp(f.calls,"IGDX")==0);
}
static void snapshots_are_values() {
    Fixture first; first.language=12;
    const auto a=system_language_horizon_read(port(first)); const auto saved=a.snapshot;
    Fixture second; second.language=1; const auto b=system_language_horizon_read(port(second));
    assert(saved.language==SystemLanguage::EnglishGB && b.snapshot.language==SystemLanguage::EnglishUS);
    auto child=saved; child.language=SystemLanguage::Japanese;
    assert(a.snapshot.language==SystemLanguage::EnglishGB && child.language==SystemLanguage::Japanese);
    assert(SystemLanguageSnapshot{}.status==SystemLanguageStatus::Unavailable);
}
int main() {
    mappings(); failures_and_unsupported(); invalid_ports_and_capture(); snapshots_are_values();
    std::puts("SYSTEM_LANGUAGE_HORIZON_PASS mapped18=1 lifecycle_balanced=1 failures_truthful=1 unknown_unsupported=1 immutable_values=1");
}
