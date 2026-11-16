#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t DISC_CHANGE_NID=0xdfdd302e;
bool same_disc_pair(ps3_disc_change_pair a,ps3_disc_change_pair b) {
    return a.eject_descriptor==b.eject_descriptor && a.insert_descriptor==b.insert_descriptor;
}
struct DiscGuard {
    ps3_pthread_guard mutex{};
    StartupControl *control=nullptr;
    bool cancel_after_unlock=false;
    DiscGuard() {CHECK(ps3_pthread_guard_init(&mutex)==0);}
    ~DiscGuard() {ps3_pthread_guard_destroy(&mutex);}
    static void lock(void *p) {auto &s=*static_cast<DiscGuard*>(p);auto g=ps3_pthread_guard_port(&s.mutex);g.lock(g.context);}
    static void unlock(void *p) {
        auto &s=*static_cast<DiscGuard*>(p);auto g=ps3_pthread_guard_port(&s.mutex);g.unlock(g.context);
        if(s.cancel_after_unlock)s.control->cancelled.store(true);
    }
    ps3_instance_guard port() {return {this,lock,unlock};}
};
struct DiscFixture {
    DiscGuard guard;
    ps3_disc_change_registry registry{};
    Runtime runtime;
    DiscFixture() {
        guard.control=runtime.control.get();
        CHECK(ps3_disc_change_init(&registry,guard.port(),PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_OK);
        runtime.execution.disc_change_callbacks=&registry;
        const unsigned char sentinel[8]{0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};runtime.put(TOC_SLOT,sentinel,8);
    }
    ps3_disc_change_pair pair() {
        ps3_disc_change_pair value{};CHECK(ps3_disc_change_read(&registry,&value)==PS3_DISC_CHANGE_OK);return value;
    }
    void query(uint64_t eject=0x7738e0,uint64_t insert=0x7738e8,uint64_t stack=STACK) {
        auto &cpu=runtime.execution.ppu.cpu;cpu.gpr[1]=stack;cpu.gpr[2]=TOC;cpu.gpr[3]=eject;cpu.gpr[4]=insert;
        cpu.gpr[5]=UINT64_C(0x123456789abcdef0);runtime.run(DISC_CHANGE_NID,false);
    }
};
struct DiscMemoryObservation {
    ps3_guest_memory real{};
    StartupControl *control=nullptr;
    unsigned reads=0,writes=0;
    bool fail_read=false,fail_write=false,cancel_after_write=false;
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n) {
        auto &s=*static_cast<DiscMemoryObservation*>(p);++s.reads;
        if(s.fail_read)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        return s.real.read(s.real.context,a,v,n);
    }
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n) {
        auto &s=*static_cast<DiscMemoryObservation*>(p);++s.writes;
        if(s.fail_write)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.write(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.cancel_after_write)s.control->cancelled.store(true);
        return result;
    }
    void install(ps3_guest_memory &memory) {real=memory;memory.context=this;memory.read=read;memory.write=write;}
};
void real_registration_and_replacement() {
    DiscFixture f;auto &r=f.runtime;
    const ps3_disc_change_pair cases[]={{0x7738e0,0x7738e8},{0,0},{UINT32_MAX,0x90000001},{UINT32_MAX,0x90000001},{1,0}};
    ps3_disc_change_pair previous{};
    for(const auto value:cases) {
        DiscMemoryObservation memory;memory.control=r.control.get();memory.install(r.guest.backing);
        const auto from=r.published();
        f.query(UINT64_C(0xabc1000000000000)|value.eject_descriptor,UINT64_C(0xdef2000000000000)|value.insert_descriptor);
        r.guest.backing=memory.real;
        const auto &o=r.control->disc_change;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
        CHECK(r.execution.ppu.cpu.gpr[4]==(UINT64_C(0xdef2000000000000)|value.insert_descriptor));
        CHECK(r.execution.ppu.cpu.gpr[5]==UINT64_C(0x123456789abcdef0));
        CHECK(o.raw_eject==(UINT64_C(0xabc1000000000000)|value.eject_descriptor));
        CHECK(o.raw_insert==(UINT64_C(0xdef2000000000000)|value.insert_descriptor));
        CHECK(o.registration_attempted && o.registration.status==PS3_DISC_CHANGE_OK && o.registration.committed);
        CHECK(same_disc_pair(o.registration.previous,previous) && same_disc_pair(o.registration.current,value));
        CHECK(same_disc_pair(f.pair(),value) && o.toc_written && o.has_result && o.publications==1);
        CHECK(memory.reads==1 && memory.writes==1); // TOC only, even for unmapped callback descriptors.
        const auto events=r.since(from);CHECK(events.size()==1);
        if(events.size()==1)CHECK(is_event(events[0],true,TOC_SLOT,8,TOC));
        previous=value;
    }
    CHECK(r.control->disc_change_calls==5 && r.control->disc_change_returns==5);
    CHECK(r.control->system_string_calls==0 && r.control->system_parameter_calls==0 && r.control->boot_disc_calls==0);
}
void unavailable_and_precommit_stops() {
    for(unsigned scenario=0;scenario<7;++scenario) {
        DiscFixture f;auto &r=f.runtime;ps3_disc_change_registry unavailable{};
        DiscMemoryObservation memory;memory.control=r.control.get();
        uint64_t stack=STACK;StartupStop expected=StartupStop::InvalidMemory;
        switch(scenario) {
        case 0:r.execution.disc_change_callbacks=nullptr;expected=StartupStop::MissingServices;break;
        case 1:r.execution.disc_change_callbacks=&unavailable;expected=StartupStop::DiscChangeStop;break;
        case 2:r.leave_trace_capacity(0);expected=StartupStop::TraceCapacity;break;
        case 3:stack=UINT64_MAX-0x20;break;
        case 4:stack=UINT32_MAX;break;
        case 5:memory.fail_read=true;memory.install(r.guest.backing);break;
        case 6:r.control->cancelled.store(true);expected=StartupStop::Cancelled;break;
        }
        f.query(0x7738e0,0x7738e8,stack);
        if(scenario==5)r.guest.backing=memory.real;
        CHECK(r.control->stop==expected && !r.control->disc_change.has_result && r.control->disc_change_returns==0);
        CHECK(!r.control->disc_change.registration.committed && !r.control->disc_change.toc_written);
        CHECK(same_disc_pair(f.pair(),{0,0}) && r.get(TOC_SLOT,8)==UINT64_C(0xa5a5a5a5a5a5a5a5));
    }
}
void committed_registration_survives_terminal_boundaries() {
    for(unsigned scenario=0;scenario<3;++scenario) {
        DiscFixture f;auto &r=f.runtime;
        CHECK(ps3_disc_change_register(&f.registry,{11,22}).committed);
        DiscMemoryObservation memory;memory.control=r.control.get();
        if(scenario==0)f.guard.cancel_after_unlock=true;
        if(scenario==1){memory.fail_write=true;memory.install(r.guest.backing);}
        if(scenario==2){memory.cancel_after_write=true;memory.install(r.guest.backing);}
        f.query();f.guard.cancel_after_unlock=false;
        if(scenario!=0)r.guest.backing=memory.real;
        const auto &o=r.control->disc_change;
        CHECK(r.control->stop==(scenario==1?StartupStop::ProviderFailure:StartupStop::Cancelled));
        CHECK(o.registration_attempted && o.registration.committed && o.registration.status==PS3_DISC_CHANGE_OK);
        CHECK(same_disc_pair(o.registration.previous,{11,22}) && same_disc_pair(f.pair(),{0x7738e0,0x7738e8}));
        CHECK(!o.has_result && r.control->disc_change_returns==0 && o.toc_written==(scenario==2));
        CHECK(r.get(TOC_SLOT,8)==(scenario==2?TOC:UINT64_C(0xa5a5a5a5a5a5a5a5)));
        r.control->cancelled.store(false);f.query(7,8);
        CHECK(r.control->stop==StartupStop::None && r.control->disc_change.has_result);
        CHECK(same_disc_pair(r.control->disc_change.registration.previous,{0x7738e0,0x7738e8}));
        CHECK(same_disc_pair(f.pair(),{7,8}) && r.control->disc_change_calls==2 && r.control->disc_change_returns==1);
    }
}
}
int main() {
    real_registration_and_replacement();unavailable_and_precommit_stops();committed_registration_survives_terminal_boundaries();
    std::printf("DISC_CHANGE_HLE_FAILURES=%d\n",failures);return failures?1:0;
}
