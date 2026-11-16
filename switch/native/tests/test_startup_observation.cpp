#include "startup_observation.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
using namespace infamous::diagnostics;
static unsigned checks, failures, scenarios, deaths;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL line=%d expression=%s\n",__LINE__,#value); } } while (0)
static_assert(!std::is_copy_constructible<StartupObservation>::value);
static_assert(!std::is_copy_assignable<StartupObservation>::value);
static_assert(!std::is_move_constructible<StartupObservation>::value);
static_assert(!std::is_move_assignable<StartupObservation>::value);
static_assert(std::is_trivially_copyable<DispatchTicket>::value);
static_assert(std::is_trivially_destructible<StartupObservation>::value);
static DispatchEntry entry(std::uint32_t depth) {
    return {UINT64_C(0xabcdef1200000400)+depth,UINT64_C(0xfedcba9800000800)+depth,UINT64_C(0x8765432100001000)+depth,depth};
}
static DispatchResult result(std::uint32_t value=17) { return {value,true,0xabc00000u+value}; }
static bool same_entry(DispatchEntry a,DispatchEntry b) {
    return a.captured_ctr==b.captured_ctr && a.lr==b.lr && a.sp==b.sp && a.depth==b.depth;
}
static bool same_result(DispatchResult a,DispatchResult b) {
    return a.status==b.status && a.lookup_attempted==b.lookup_attempted && a.lookup_status==b.lookup_status;
}
static bool same_pending(PendingDispatch a,PendingDispatch b) {
    return a.call_id==b.call_id && a.parent_id==b.parent_id && same_entry(a.entry,b.entry) && a.occupied==b.occupied;
}
static bool same_completed(CompletedDispatch a,CompletedDispatch b) {
    return a.sequence==b.sequence && a.call_id==b.call_id && a.parent_id==b.parent_id && same_entry(a.entry,b.entry) && a.depth_after==b.depth_after && same_result(a.result,b.result);
}
struct Fixture {
    ImportRecord imports[64]{};
    CompletedDispatch completed[32]{};
    PendingDispatch pending[8]{};
    StartupObservation observer;
    bool configure(std::size_t imports_count=64,std::size_t completed_count=32,std::size_t pending_count=8) {
        const bool ok=observer.configure(imports,imports_count,completed,completed_count,pending,pending_count);
        CHECK(ok); return ok;
    }
};
static void check_range(SuffixRange range,std::uint64_t total,std::uint64_t begin,std::size_t capacity) {
    CHECK(range.total==total); CHECK(range.retained_begin==begin); CHECK(range.retained_end==total);
    CHECK(range.overwritten_end==begin); CHECK(range.capacity==capacity);
}
static void configuration() {
    ++scenarios; Fixture f; CHECK(!f.observer.valid_for_admission());
    for(unsigned n=0;n<64;++n) f.imports[n]={77+n,0xaabbu+n};
    for(unsigned n=0;n<32;++n) f.completed[n]={88+n,99+n,111+n,entry(n),n,result(n)};
    for(unsigned n=0;n<8;++n) f.pending[n]={122+n,133+n,entry(n),true};
    auto reject=[&](ImportRecord *ip,std::size_t ic,CompletedDispatch *cp,std::size_t cc,PendingDispatch *pp,std::size_t pc) {
        CHECK(!f.observer.configure(ip,ic,cp,cc,pp,pc)); CHECK(!f.observer.report().configured);
        for(unsigned n=0;n<64;++n) CHECK(f.imports[n].sequence==77+n && f.imports[n].nid==0xaabbu+n);
        for(unsigned n=0;n<32;++n) CHECK(same_completed(f.completed[n],{88+n,99+n,111+n,entry(n),n,result(n)}));
        for(unsigned n=0;n<8;++n) CHECK(same_pending(f.pending[n],{122+n,133+n,entry(n),true}));
    };
    reject(nullptr,1,f.completed,1,f.pending,1); reject(f.imports,0,f.completed,1,f.pending,1);
    reject(f.imports,1,nullptr,1,f.pending,1); reject(f.imports,1,f.completed,0,f.pending,1);
    reject(f.imports,1,f.completed,1,nullptr,1); reject(f.imports,1,f.completed,1,f.pending,0);
    reject(reinterpret_cast<ImportRecord *>(reinterpret_cast<std::uintptr_t>(f.imports)+1),1,f.completed,1,f.pending,1);
    reject(f.imports,1,reinterpret_cast<CompletedDispatch *>(reinterpret_cast<std::uintptr_t>(f.completed)+1),1,f.pending,1);
    reject(f.imports,1,f.completed,1,reinterpret_cast<PendingDispatch *>(reinterpret_cast<std::uintptr_t>(f.pending)+1),1);
    reject(f.imports,1,reinterpret_cast<CompletedDispatch *>(f.imports),1,f.pending,1);
    reject(reinterpret_cast<ImportRecord *>(f.pending),1,f.completed,1,f.pending,1);
    reject(f.imports,1,f.completed,1,reinterpret_cast<PendingDispatch *>(f.completed),1);
    reject(reinterpret_cast<ImportRecord *>(&f.observer),1,f.completed,1,f.pending,1);
    reject(f.imports,1,reinterpret_cast<CompletedDispatch *>(&f.observer),1,f.pending,1);
    reject(f.imports,1,f.completed,1,reinterpret_cast<PendingDispatch *>(&f.observer),1);
    reject(f.imports,std::numeric_limits<std::size_t>::max()/sizeof(ImportRecord)+1,f.completed,1,f.pending,1);
    reject(f.imports,1,f.completed,std::numeric_limits<std::size_t>::max()/sizeof(CompletedDispatch)+1,f.pending,1);
    reject(reinterpret_cast<ImportRecord *>(std::numeric_limits<std::uintptr_t>::max() & ~(std::uintptr_t(alignof(ImportRecord))-1)),1,f.completed,1,f.pending,1);
    if(std::numeric_limits<std::size_t>::max()>UINT32_MAX) reject(f.imports,1,f.completed,1,f.pending,std::size_t(UINT32_MAX)+1);
    if(!f.configure()) return;
    CHECK(f.observer.valid_for_admission()); for(const auto &slot:f.pending) CHECK(!slot.occupied);
    f.pending[7].occupied=true; CHECK(!f.observer.valid_for_admission());
    f.pending[7].occupied=false; CHECK(f.observer.valid_for_admission());
    CHECK(f.imports[0].sequence==77 && f.completed[0].sequence==88);
    auto report=f.observer.report(); CHECK(report.configured && report.dispatch_started==0 && report.pending_count==0 && !report.immediate_outstanding && report.pending_capacity==8);
    check_range(report.imports,0,0,64); check_range(report.completed,0,0,32);
    CHECK(f.observer.try_record_import(0x12345678)); CHECK(!f.observer.valid_for_admission());
    CHECK(!f.observer.configure(f.imports,1,f.completed,1,f.pending,1));
    check_range(f.observer.report().imports,1,0,64); CHECK(f.imports[0].nid==0x12345678);
}
static void import_wrap() {
    ++scenarios; Fixture f; if(!f.configure(1,1,1)) return;
    CHECK(f.observer.try_record_import(0x11111111)); CHECK(f.observer.try_record_import(0x22222222));
    ImportRecord out{123,456}; CHECK(!f.observer.read_import(0,out)); CHECK(out.sequence==123 && out.nid==456);
    CHECK(f.observer.read_import(1,out)); CHECK(out.sequence==1 && out.nid==0x22222222); check_range(f.observer.report().imports,2,1,1);
    ++scenarios; Fixture full; if(!full.configure()) return;
    for(std::uint32_t n=0;n<70;++n) CHECK(full.observer.try_record_import(0x80000000u+n));
    check_range(full.observer.report().imports,70,6,64);
    for(std::uint64_t n=6;n<70;++n) { CHECK(full.observer.read_import(n,out)); CHECK(out.sequence==n && out.nid==0x80000000u+n); }
    out={99,88}; CHECK(!full.observer.read_import(70,out)); CHECK(out.sequence==99 && out.nid==88);
    CHECK(!full.observer.read_import(UINT64_MAX,out)); CHECK(out.sequence==99 && out.nid==88);
}
static void nested_and_wrap() {
    ++scenarios; Fixture f; if(!f.configure(4,4,8)) return;
    DispatchTicket tickets[3];
    for(unsigned n=0;n<3;++n) {
        CHECK(f.observer.try_begin_dispatch(entry(n),DispatchStorage::Pending,tickets[n]));
        PendingDispatch pending{}; CHECK(f.observer.read_pending(n,pending));
        CHECK(pending.call_id==n && pending.parent_id==(n?n-1:NO_PARENT_ID) && pending.occupied && same_entry(pending.entry,entry(n)));
    }
    CHECK(f.observer.report().pending_count==3 && f.observer.report().dispatch_started==3);
    for(unsigned n=3;n>0;--n) { DispatchTicket copied=tickets[n-1]; f.observer.finish_dispatch(copied,n-1,result(n)); }
    for(unsigned seq=0;seq<3;++seq) {
        CompletedDispatch out{}; CHECK(f.observer.read_completed(seq,out)); const unsigned id=2-seq;
        CHECK(out.sequence==seq && out.call_id==id && out.parent_id==(id?id-1:NO_PARENT_ID));
        CHECK(same_entry(out.entry,entry(id)) && out.depth_after==id && same_result(out.result,result(id+1)));
    }
    CHECK(f.observer.report().pending_count==0 && !f.observer.valid_for_admission());
    PendingDispatch sentinel{91,92,entry(7),true},copy=sentinel;
    CHECK(!f.observer.read_pending(0,sentinel) && same_pending(sentinel,copy));
    CHECK(!f.observer.read_pending(8,sentinel) && same_pending(sentinel,copy));
    ++scenarios; Fixture wrap; if(!wrap.configure(1,1,1)) return;
    DispatchTicket outer,inner; CHECK(wrap.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,outer));
    CHECK(wrap.observer.try_begin_dispatch(entry(1),DispatchStorage::ImmediateNonInvoking,inner));
    wrap.observer.finish_dispatch(inner,1,{4,false,0xfeed}); wrap.observer.finish_dispatch(outer,0,result());
    check_range(wrap.observer.report().completed,2,1,1); CompletedDispatch out{}; CHECK(wrap.observer.read_completed(1,out)); CHECK(out.call_id==0 && out.sequence==1 && out.parent_id==NO_PARENT_ID);
    ++scenarios; Fixture serial; if(!serial.configure()) return;
    for(unsigned n=0;n<40;++n) { DispatchTicket ticket; CHECK(serial.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,ticket)); serial.observer.finish_dispatch(ticket,0,result(n)); }
    check_range(serial.observer.report().completed,40,8,32); CHECK(serial.observer.report().dispatch_started==40);
    for(unsigned n=8;n<40;++n) { CHECK(serial.observer.read_completed(n,out)); CHECK(out.call_id==n && out.sequence==n && out.parent_id==NO_PARENT_ID && same_result(out.result,result(n))); }
    CompletedDispatch saved{91,92,93,entry(4),4,result(7)},unchanged=saved;
    CHECK(!serial.observer.read_completed(7,saved) && same_completed(saved,unchanged));
    CHECK(!serial.observer.read_completed(40,saved) && same_completed(saved,unchanged));
}
static void immediate_and_retention() {
    ++scenarios; Fixture f; if(!f.configure()) return;
    DispatchTicket ancestors[8],immediate;
    for(unsigned n=0;n<8;++n) CHECK(f.observer.try_begin_dispatch(entry(n),DispatchStorage::Pending,ancestors[n]));
    CHECK(f.observer.try_begin_dispatch(entry(8),DispatchStorage::ImmediateNonInvoking,immediate));
    auto report=f.observer.report(); CHECK(report.dispatch_started==9 && report.pending_count==8 && report.immediate_outstanding); check_range(report.completed,0,0,32);
    for(unsigned n=0;n<8;++n) { PendingDispatch out{}; CHECK(f.observer.read_pending(n,out)); CHECK(out.call_id==n && out.parent_id==(n?n-1:NO_PARENT_ID)); }
    f.observer.finish_dispatch(immediate,8,{4,false,0xaabbccdd});
    report=f.observer.report(); CHECK(!report.immediate_outstanding && report.pending_count==8 && report.dispatch_started==9);
    CompletedDispatch complete{}; CHECK(f.observer.read_completed(0,complete));
    CHECK(complete.call_id==8 && complete.parent_id==7 && complete.depth_after==8 && same_entry(complete.entry,entry(8)));
    CHECK(complete.result.status==4 && !complete.result.lookup_attempted && complete.result.lookup_status==0xaabbccdd);
    for(unsigned n=8;n>0;--n) f.observer.finish_dispatch(ancestors[n-1],n-1,result(n));
    CHECK(f.observer.report().completed.total==9 && f.observer.report().pending_count==0);
    ++scenarios; Fixture corrupt; if(!corrupt.configure()) return;
    DispatchTicket ticket; CHECK(corrupt.observer.try_begin_dispatch(entry(UINT32_MAX),DispatchStorage::ImmediateNonInvoking,ticket));
    corrupt.observer.finish_dispatch(ticket,UINT32_MAX,{2,false,17});
    CHECK(corrupt.observer.read_completed(0,complete)); CHECK(complete.parent_id==NO_PARENT_ID && complete.entry.depth==UINT32_MAX);
}
static void failed_reads_and_helper() {
    ++scenarios; StartupObservation observer; ImportRecord import{7,8}; CHECK(!observer.read_import(0,import) && import.sequence==7 && import.nid==8);
    PendingDispatch pending{9,10,entry(1),true},before=pending; CHECK(!observer.read_pending(0,pending) && same_pending(pending,before));
    CompletedDispatch completed{11,12,13,entry(2),2,result(3)},saved=completed; CHECK(!observer.read_completed(0,completed) && same_completed(completed,saved));
    std::uint64_t next=55; CHECK(startup_observation_checked_increment(UINT64_MAX-1,next) && next==UINT64_MAX);
    next=55; CHECK(!startup_observation_checked_increment(UINT64_MAX,next) && next==55);
    CHECK(startup_observation_checked_increment(0,next) && next==1);
}
static void aliased_read_outputs() {
    ++scenarios; Fixture f; if(!f.configure()) return;
    CHECK(f.observer.try_record_import(0x11111111)); CHECK(f.observer.try_record_import(0x22222222));
    const ImportRecord import_before=f.imports[1];
    CHECK(!f.observer.read_import(0,f.imports[1]));
    CHECK(f.imports[1].sequence==import_before.sequence && f.imports[1].nid==import_before.nid);
    CHECK(!f.observer.read_import(0,f.imports[0]));
    ImportRecord import_copy{}; CHECK(f.observer.read_import(0,import_copy)); CHECK(import_copy.sequence==0 && import_copy.nid==0x11111111);
    for(unsigned n=0;n<2;++n) { DispatchTicket ticket; CHECK(f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,ticket)); f.observer.finish_dispatch(ticket,0,result(n)); }
    const CompletedDispatch completed_before=f.completed[1];
    CHECK(!f.observer.read_completed(0,f.completed[1])); CHECK(same_completed(f.completed[1],completed_before));
    CHECK(!f.observer.read_completed(0,f.completed[0]));
    CompletedDispatch completed_copy{}; CHECK(f.observer.read_completed(0,completed_copy)); CHECK(completed_copy.call_id==0 && completed_copy.sequence==0 && same_result(completed_copy.result,result(0)));
    DispatchTicket root,child; CHECK(f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,root)); CHECK(f.observer.try_begin_dispatch(entry(1),DispatchStorage::Pending,child));
    const PendingDispatch pending_before=f.pending[1];
    const bool alias_accepted=f.observer.read_pending(0,f.pending[1]);
    CHECK(!alias_accepted); CHECK(same_pending(f.pending[1],pending_before));
    CHECK(!f.observer.read_pending(0,f.pending[0]));
    PendingDispatch pending_copy{}; CHECK(f.observer.read_pending(0,pending_copy)); CHECK(pending_copy.call_id==2 && pending_copy.parent_id==NO_PARENT_ID);
    const auto report=f.observer.report(); CHECK(report.dispatch_started==4 && report.pending_count==2 && report.completed.total==2 && report.imports.total==2);
    // A broken read may have corrupted a pending slot; do not fabricate a
    // valid completion to hide the failure or trigger an unrelated abort.
    if(!alias_accepted && same_pending(f.pending[1],pending_before)) {
        f.observer.finish_dispatch(child,1,result()); f.observer.finish_dispatch(root,0,result());
        CHECK(f.observer.report().pending_count==0 && f.observer.report().completed.total==4);
    }
}
enum class Misuse {
    UnconfiguredImport, UnconfiguredBegin, InvalidStorage, MissingParent, DefaultTicket, OccupiedDepth,
    ForeignOwner, WrongDepthAfter, DuplicatePending, StalePending, NonLifoPending, NestedImmediate,
    PendingWhileImmediate, BeginWhileImmediate, WrongImmediateDepth, DuplicateImmediate,
    UnconfiguredFinish, CorruptOccupiedSlot, CorruptParentSlot, StaleImmediate
};
static void misuse(Misuse scenario) {
    Fixture f,g; DispatchTicket root,child,immediate;
    if(scenario==Misuse::UnconfiguredImport) { f.observer.try_record_import(1); return; }
    if(scenario==Misuse::UnconfiguredBegin) { f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,root); return; }
    if(scenario==Misuse::UnconfiguredFinish) { f.observer.finish_dispatch(root,0,result()); return; }
    if(!f.configure() || !g.configure()) _exit(3);
    if(scenario==Misuse::InvalidStorage) { f.observer.try_begin_dispatch(entry(0),static_cast<DispatchStorage>(255),root); return; }
    if(scenario==Misuse::MissingParent) { f.observer.try_begin_dispatch(entry(1),DispatchStorage::Pending,root); return; }
    if(scenario==Misuse::DefaultTicket) { f.observer.finish_dispatch(root,0,result()); return; }
    if(scenario==Misuse::CorruptOccupiedSlot) { f.pending[0].occupied=true; f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,root); return; }
    f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,root);
    if(scenario==Misuse::OccupiedDepth) { f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,child); return; }
    if(scenario==Misuse::ForeignOwner) { DispatchTicket other; g.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,other); g.observer.finish_dispatch(root,0,result()); return; }
    if(scenario==Misuse::CorruptParentSlot) { f.pending[0].occupied=false; f.observer.try_begin_dispatch(entry(1),DispatchStorage::Pending,child); return; }
    if(scenario==Misuse::WrongDepthAfter) { f.observer.finish_dispatch(root,1,result()); return; }
    if(scenario==Misuse::DuplicatePending) { f.observer.finish_dispatch(root,0,result()); f.observer.finish_dispatch(root,0,result()); return; }
    if(scenario==Misuse::StalePending) { f.observer.finish_dispatch(root,0,result()); f.observer.try_begin_dispatch(entry(0),DispatchStorage::Pending,child); f.observer.finish_dispatch(root,0,result()); return; }
    if(scenario==Misuse::NonLifoPending) { f.observer.try_begin_dispatch(entry(1),DispatchStorage::Pending,child); f.observer.finish_dispatch(root,0,result()); return; }
    f.observer.try_begin_dispatch(entry(1),DispatchStorage::ImmediateNonInvoking,immediate);
    if(scenario==Misuse::NestedImmediate) { f.observer.try_begin_dispatch(entry(1),DispatchStorage::ImmediateNonInvoking,child); return; }
    if(scenario==Misuse::PendingWhileImmediate) { f.observer.finish_dispatch(root,0,result()); return; }
    if(scenario==Misuse::BeginWhileImmediate) { f.observer.try_begin_dispatch(entry(1),DispatchStorage::Pending,child); return; }
    if(scenario==Misuse::WrongImmediateDepth) { f.observer.finish_dispatch(immediate,2,result()); return; }
    if(scenario==Misuse::StaleImmediate) { f.observer.finish_dispatch(immediate,1,result()); f.observer.try_begin_dispatch(entry(1),DispatchStorage::ImmediateNonInvoking,child); f.observer.finish_dispatch(immediate,1,result()); return; }
    f.observer.finish_dispatch(immediate,1,result()); f.observer.finish_dispatch(immediate,1,result());
}
static void death_checks() {
    ++scenarios; std::fflush(nullptr);
    struct Case { Misuse scenario; const char *name; };
    const Case cases[] = {
        {Misuse::UnconfiguredImport,"UnconfiguredImport"},
        {Misuse::UnconfiguredBegin,"UnconfiguredBegin"},
        {Misuse::InvalidStorage,"InvalidStorage"},
        {Misuse::MissingParent,"MissingParent"},
        {Misuse::DefaultTicket,"DefaultTicket"},
        {Misuse::OccupiedDepth,"OccupiedDepth"},
        {Misuse::ForeignOwner,"ForeignOwner"},
        {Misuse::WrongDepthAfter,"WrongDepthAfter"},
        {Misuse::DuplicatePending,"DuplicatePending"},
        {Misuse::StalePending,"StalePending"},
        {Misuse::NonLifoPending,"NonLifoPending"},
        {Misuse::NestedImmediate,"NestedImmediate"},
        {Misuse::PendingWhileImmediate,"PendingWhileImmediate"},
        {Misuse::BeginWhileImmediate,"BeginWhileImmediate"},
        {Misuse::WrongImmediateDepth,"WrongImmediateDepth"},
        {Misuse::DuplicateImmediate,"DuplicateImmediate"},
        {Misuse::UnconfiguredFinish,"UnconfiguredFinish"},
        {Misuse::CorruptOccupiedSlot,"CorruptOccupiedSlot"},
        {Misuse::CorruptParentSlot,"CorruptParentSlot"},
        {Misuse::StaleImmediate,"StaleImmediate"},
    };
    for(const auto &test:cases) {
        const pid_t child=fork(); CHECK(child>=0); if(child<0) return;
        if(child==0) { misuse(test.scenario); _exit(0); }
        int status=0; CHECK(waitpid(child,&status,0)==child); CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT); ++deaths;
        std::printf("STARTUP_OBSERVATION_MISUSE name=%s signal=%d\n",test.name,WIFSIGNALED(status)?WTERMSIG(status):0);
    }
}
int main() {
    configuration(); import_wrap(); nested_and_wrap(); immediate_and_retention(); failed_reads_and_helper(); aliased_read_outputs(); death_checks();
    std::printf("STARTUP_OBSERVATION_%s scenarios=%u checks=%u failures=%u misuse_children=%u pure_storage_only=true\n",failures?"FAIL":"PASS",scenarios,checks,failures,deaths);
    return failures?1:0;
}
