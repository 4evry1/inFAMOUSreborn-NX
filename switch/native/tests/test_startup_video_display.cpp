/* Focused host test for the native display bridge routes through the REAL
 * ps3_hle_call dispatcher and the owned StartupExecution/Runtime. This tests
 * the bridge policy, not the game, hardware, a monitor or a rendered frame. */
#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t DEVICE_INFO_NID=0x1e930eef;
constexpr uint32_t AVAILABILITY_NID=0xa322db75;
constexpr uint32_t RESOLUTION_NID=0xe558748d;
constexpr uint32_t STATE_NID=0x887572d5;
constexpr uint64_t SENTINEL=UINT64_C(0xa5a5a5a5a5a5a5a5);
constexpr uint32_t INFO=STACK+0xD8;
constexpr uint32_t OUT=STACK_BASE+0x600;
// The bound func_00115870 caller stores its GetState record at SP+C8 and consumes
// only byte +8 (resolutionId) and +0x0B (aspect). Both admitted call sites are listed.
constexpr uint32_t STATE=STACK+0xC8;
// The second bound func_003AA898 caller stores its GetState record at SP+0x7C and
// consumes resolutionId (+8), scan mode (+9) and aspect (+0x0B).
constexpr uint32_t STATE_NEW=STACK+0x7C;
constexpr size_t STATE_BYTES=16;
constexpr uint32_t STATE_ID_OFFSET=0x08, STATE_SCAN_OFFSET=0x09, STATE_ASPECT_OFFSET=0x0B;
constexpr uint32_t STATE_LR_FIRST=0x00115a7c, STATE_LR_SECOND=0x00115ae4;
constexpr uint32_t STATE_NEW_LR_FIRST=0x003aa9c0, STATE_NEW_LR_SECOND=0x003aab58;
// DeviceInfo layout consumed by the real caller (hex): count, resolutionId, aspect.
constexpr uint32_t INFO_COUNT=0x04, INFO_ID=0x20, INFO_ASPECT=0x23;
void seed_toc(Runtime &r) {const unsigned char b[8]{0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};r.put(TOC_SLOT,b,8);}
void seed_info(Runtime &r,unsigned char value) {std::vector<unsigned char> b(0x1c8,value);r.put(INFO,b.data(),b.size());}
void configured(Runtime &r) {
    r.execution.presentation_topology=PresentationTopology::SingleNativeTarget;
    r.execution.presentation_mode=NativePresentationMode::Fixed720p;
}
struct Probe {
    ps3_guest_memory *installed=nullptr,real{};
    StartupControl *control=nullptr;
    unsigned reads=0,writes=0,other=0;
    int fail_read=-1,fail_write=-1,cancel_after_read=-1,cancel_after_write=-1;
    explicit Probe(Runtime &r):installed(&r.guest.backing),real(*installed),control(r.control.get()) {
        *installed={this,commit,decommit,read,write,zero};
    }
    ~Probe(){restore();}
    void restore(){if(installed){*installed=real;installed=nullptr;}}
    static ps3_guest_memory_status commit(void *p,uint32_t a,uint64_t n){auto&s=*static_cast<Probe*>(p);++s.other;return s.real.commit(s.real.context,a,n);}
    static ps3_guest_memory_status decommit(void *p,uint32_t a,uint64_t n){auto&s=*static_cast<Probe*>(p);++s.other;return s.real.decommit(s.real.context,a,n);}
    static ps3_guest_memory_status zero(void *p,uint32_t a,uint64_t n){auto&s=*static_cast<Probe*>(p);++s.other;return s.real.zero(s.real.context,a,n);}
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n){auto&s=*static_cast<Probe*>(p);
        const int index=int(s.reads++);if(index==s.fail_read)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto r=s.real.read(s.real.context,a,v,n);if(r==PS3_GUEST_MEMORY_OK&&index==s.cancel_after_read)s.control->cancelled.store(true);return r;}
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n){auto&s=*static_cast<Probe*>(p);
        const int index=int(s.writes++);if(index==s.fail_write)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto r=s.real.write(s.real.context,a,v,n);if(r==PS3_GUEST_MEMORY_OK&&index==s.cancel_after_write)s.control->cancelled.store(true);return r;}
};
void call_device_info(Runtime &r,uint64_t selector=0,uint64_t index=0,uint64_t output=INFO,uint64_t lr=0x00115980) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=selector;cpu.gpr[4]=index;cpu.gpr[5]=output;
    for(unsigned i=6;i<=10;++i)cpu.gpr[i]=UINT64_C(0xffffaaaa00000000)+i;
    cpu.lr=lr;cpu.cr=0x12345678;r.run(DEVICE_INFO_NID,false);
}
void call_availability(Runtime &r,uint64_t selector,uint64_t id,uint64_t aspect,uint64_t option=UINT64_C(0xdeadbeef)) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=selector;cpu.gpr[4]=id;cpu.gpr[5]=aspect;cpu.gpr[6]=option;
    cpu.lr=0x1159cc;cpu.cr=0x12345678;r.run(AVAILABILITY_NID,false);
}
void call_resolution(Runtime &r,uint64_t id,uint64_t output) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=id;cpu.gpr[4]=output;
    cpu.lr=0x1159ec;cpu.cr=0x12345678;r.run(RESOLUTION_NID,false);
}
void call_state(Runtime &r,uint64_t selector=0,uint64_t index=0,uint64_t output=STATE,uint64_t lr=STATE_LR_FIRST,uint64_t sp=STACK) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=sp;cpu.gpr[2]=TOC;cpu.gpr[3]=selector;cpu.gpr[4]=index;cpu.gpr[5]=output;
    for(unsigned i=6;i<=10;++i)cpu.gpr[i]=UINT64_C(0xffffaaaa00000000)+i;
    cpu.lr=lr;cpu.cr=0x12345678;r.run(STATE_NID,false);
}
void seed_state(Runtime &r,unsigned char value) {std::vector<unsigned char> b(STATE_BYTES,value);r.put(STATE,b.data(),b.size());}
std::vector<unsigned char> read_state(Runtime &r) {
    std::vector<unsigned char> b(STATE_BYTES);
    CHECK(r.mem().read(r.mem().context,STATE,b.data(),b.size())==PS3_GUEST_MEMORY_OK);
    return b;
}
void seed_state_at(Runtime &r,uint32_t address,unsigned char value) {std::vector<unsigned char> b(STATE_BYTES,value);r.put(address,b.data(),b.size());}
std::vector<unsigned char> read_state_at(Runtime &r,uint32_t address) {
    std::vector<unsigned char> b(STATE_BYTES);
    CHECK(r.mem().read(r.mem().context,address,b.data(),b.size())==PS3_GUEST_MEMORY_OK);
    return b;
}
void state_success_and_exact_effect() {
    // Both admitted LRs of the single bound caller, one ordered two-byte policy.
    const uint64_t lrs[]={STATE_LR_FIRST,STATE_LR_SECOND};
    for(const uint64_t lr:lrs) {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0xa5);
        const auto before=read_state(r);
        const auto from=r.published();call_state(r,0,0,STATE,lr);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0 && r.execution.ppu.cpu.gpr[2]==TOC);
        CHECK(o.bridge_checked && o.topology==PresentationTopology::SingleNativeTarget && o.mode==NativePresentationMode::Fixed720p);
        CHECK(o.failure==StartupVideoStateFailure::None && o.has_result && o.toc_written);
        CHECK(o.raw_selector==0 && o.raw_index==0 && o.raw_output==STATE);
        CHECK(o.selector==0 && o.index==0 && o.output==STATE);
        CHECK(o.bytes_written==2 && o.publications==3 && o.caller==1);
        CHECK(o.lr==lr);
        CHECK(r.get(STATE+STATE_ID_OFFSET,1)==2 && r.get(STATE+STATE_ASPECT_OFFSET,1)==2);
        // The other fourteen bytes stay untouched poison.
        const auto after=read_state(r);
        for(size_t i=0;i<after.size();++i)
            if(i!=STATE_ID_OFFSET&&i!=STATE_ASPECT_OFFSET)CHECK(after[i]==before[i]);
        CHECK(r.get(TOC_SLOT,8)==TOC);
        CHECK(r.since(from).size()==3);
        CHECK(r.control->video_state_calls==1 && r.control->video_state_returns==1);
        CHECK(r.execution.ppu.cpu.lr==lr && r.execution.ppu.cpu.cr==0x12345678);
        for(unsigned i=6;i<=10;++i)CHECK(r.execution.ppu.cpu.gpr[i]==UINT64_C(0xffffaaaa00000000)+i);
        CHECK(r.execution.ppu.cpu.gpr[4]==0);
    }
}
void state_rejects_unsupported_boundaries() {
    for(unsigned s=0;s<12;++s) {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);
        switch(s) {
        case 0:r.execution.presentation_topology=PresentationTopology::Unconfigured;break;
        case 1:r.execution.presentation_topology=static_cast<PresentationTopology>(99);break;
        case 2:r.execution.presentation_mode=NativePresentationMode::Unconfigured;break;
        case 3:r.execution.presentation_mode=static_cast<NativePresentationMode>(99);break;
        default:break;
        }
        uint64_t selector=0,index=0,output=STATE,lr=STATE_LR_FIRST;
        if(s==4)lr=0x00115d94;                    // no-device/fallback GetState site
        else if(s==5)lr=0x00115ad0;               // not an admitted state site
        else if(s==6)selector=1;
        else if(s==7)index=1;
        else if(s==8)output=STATE+8;               // not SP+C8
        else if(s==9)output=UINT64_C(0x100000000)+STATE;   // high64 alias
        else if(s==10)output=UINT64_MAX;
        else if(s==11)output=0;                   // full-64 NULL output
        call_state(r,selector,index,output,lr);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::VideoStateStop && !o.has_result && !o.toc_written);
        CHECK(o.publications==0 && o.bytes_written==0);
        CHECK(r.get(STATE+STATE_ID_OFFSET,1)==0x5a && r.get(STATE+STATE_ASPECT_OFFSET,1)==0x5a && r.get(TOC_SLOT,8)==SENTINEL);
        CHECK(!r.control->video_state_returns && r.execution.ppu.cpu.gpr[3]==selector);
        if(s==4||s==5)CHECK(o.failure==StartupVideoStateFailure::WrongLr);
        if(s==6)CHECK(o.failure==StartupVideoStateFailure::WrongSelector);
        if(s==7)CHECK(o.failure==StartupVideoStateFailure::WrongIndex);
        if(s>=8)CHECK(o.failure==StartupVideoStateFailure::WrongOutput);
    }
}
void state_admits_high32_only_selector_and_index_aliases() {
    // Only the low 32 bits of selector/index are compared; a non-zero high half
    // is observed raw but admitted. Output is still the exact bound record.
    struct Case{uint64_t selector,index;uint64_t lr;};
    const Case cases[]={
        {UINT64_C(0x100000000),0,STATE_LR_FIRST},
        {0,UINT64_C(0x100000000),STATE_LR_SECOND},
        {UINT64_C(0xdeadbeef00000000),UINT64_C(0xffffffff00000000),STATE_LR_FIRST}};
    for(const auto &c:cases) {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0xa5);
        const auto before=read_state(r);
        const auto from=r.published();call_state(r,c.selector,c.index,STATE,c.lr);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0 && r.execution.ppu.cpu.gpr[2]==TOC);
        CHECK(o.failure==StartupVideoStateFailure::None && o.has_result && o.toc_written);
        CHECK(o.raw_selector==c.selector && o.raw_index==c.index && o.raw_output==STATE);
        CHECK(o.selector==0 && o.index==0 && o.output==STATE);
        CHECK(o.bytes_written==2 && o.publications==3 && o.caller==1);
        CHECK(o.lr==c.lr);
        CHECK(r.get(STATE+STATE_ID_OFFSET,1)==2 && r.get(STATE+STATE_ASPECT_OFFSET,1)==2);
        const auto after=read_state(r);
        for(size_t i=0;i<after.size();++i)
            if(i!=STATE_ID_OFFSET&&i!=STATE_ASPECT_OFFSET)CHECK(after[i]==before[i]);
        CHECK(r.get(TOC_SLOT,8)==TOC && r.since(from).size()==3);
        CHECK(r.control->video_state_calls==1 && r.control->video_state_returns==1);
        CHECK(r.execution.ppu.cpu.lr==c.lr);
    }
}
void state_rejects_invalid_sp_and_overflow_before_effects() {
    const uint64_t sps[]={UINT64_C(0x100000000),UINT64_C(0xffffff80),UINT64_MAX};
    for(const uint64_t sp:sps) {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);
        const auto before=read_state(r);
        call_state(r,0,0,STATE,STATE_LR_FIRST,sp);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::InvalidMemory && !o.has_result && !o.toc_written);
        CHECK(o.publications==0 && o.bytes_written==0 && !r.control->video_state_returns);
        const auto after=read_state(r);
        for(size_t i=0;i<after.size();++i)CHECK(after[i]==before[i]);
        CHECK(r.get(TOC_SLOT,8)==SENTINEL && r.execution.ppu.cpu.gpr[3]==0);
    }
}
void state_requires_whole_bound_record() {
    // SP+C8 is admitted only with all sixteen bytes of the record writable.
    Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);
    r.ranges[0]={STACK_BASE,0x9E4,true};                  // covers STATE..STATE+11 only
    r.ranges[1]={STACK_BASE+0x9E4,0x400,true};
    const auto before=read_state(r);
    call_state(r);
    const auto &o=r.control->video_state;
    CHECK(r.control->stop==StartupStop::InvalidMemory && !o.has_result && !o.toc_written);
    CHECK(o.failure==StartupVideoStateFailure::InvalidExtent && !o.publications && !o.bytes_written);
    const auto after=read_state(r);
    for(size_t i=0;i<after.size();++i)CHECK(after[i]==before[i]);
    CHECK(r.get(TOC_SLOT,8)==SENTINEL && !r.control->video_state_returns);
}
void state_failure_prefix_and_cancellation() {
    // Each failure keeps exactly the already committed prefix and never publishes a result.
    for(unsigned s=0;s<9;++s) {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);Probe p(r);
        switch(s) {
        case 0:p.fail_write=0;break;                 // first state store
        case 1:p.fail_write=1;break;                 // second state store
        case 2:p.fail_write=2;break;                 // TOC after both stores
        case 3:p.cancel_after_write=0;break;         // cancel after the first store
        case 4:p.cancel_after_write=1;break;         // cancel after the second store
        case 5:r.leave_trace_capacity(0);break;      // cannot reserve 3
        case 6:p.fail_read=0;break;                  // TOC preflight read
        case 7:r.control->cancelled.store(true);break;
        case 8:p.cancel_after_write=2;break;         // cancel after the committed TOC store
        }
        call_state(r);p.restore();
        const auto &o=r.control->video_state;
        CHECK(!o.has_result && !r.control->video_state_returns);
        if(s==0)CHECK(!o.bytes_written&&r.get(STATE+STATE_ID_OFFSET,1)==0x5a&&r.get(STATE+STATE_ASPECT_OFFSET,1)==0x5a&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==1)CHECK(o.bytes_written==1&&r.get(STATE+STATE_ID_OFFSET,1)==2&&r.get(STATE+STATE_ASPECT_OFFSET,1)==0x5a&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==2)CHECK(o.bytes_written==2&&r.get(STATE+STATE_ID_OFFSET,1)==2&&r.get(STATE+STATE_ASPECT_OFFSET,1)==2&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==3)CHECK(r.control->stop==StartupStop::Cancelled&&r.get(STATE+STATE_ID_OFFSET,1)==2&&r.get(STATE+STATE_ASPECT_OFFSET,1)==0x5a);
        if(s==4)CHECK(r.control->stop==StartupStop::Cancelled&&r.get(STATE+STATE_ID_OFFSET,1)==2&&r.get(STATE+STATE_ASPECT_OFFSET,1)==2&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==5)CHECK(r.control->stop==StartupStop::TraceCapacity);
        if(s==6)CHECK(r.control->stop==StartupStop::InvalidMemory);
        if(s==7)CHECK(r.control->stop==StartupStop::Cancelled);
        if(s==8)CHECK(r.control->stop==StartupStop::Cancelled&&o.toc_written&&o.bytes_written==2&&o.publications==2&&r.get(STATE+STATE_ID_OFFSET,1)==2&&r.get(STATE+STATE_ASPECT_OFFSET,1)==2&&r.get(TOC_SLOT,8)==TOC&&r.execution.ppu.cpu.gpr[3]==0);
        if(s==0||s==1||s==2)CHECK(r.control->stop==StartupStop::ProviderFailure);
    }
}
void state_new_caller_success_and_exact_effect() {
    // Both admitted LRs of func_003AA898: three ordered bytes at SP+0x7C.
    const uint64_t lrs[]={STATE_NEW_LR_FIRST,STATE_NEW_LR_SECOND};
    for(const uint64_t lr:lrs) {
        Runtime r;configured(r);seed_toc(r);seed_state_at(r,STATE_NEW,0xa5);
        const auto before=read_state_at(r,STATE_NEW);
        const auto from=r.published();call_state(r,0,0,STATE_NEW,lr);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0 && r.execution.ppu.cpu.gpr[2]==TOC);
        CHECK(o.bridge_checked && o.topology==PresentationTopology::SingleNativeTarget && o.mode==NativePresentationMode::Fixed720p);
        CHECK(o.failure==StartupVideoStateFailure::None && o.has_result && o.toc_written);
        CHECK(o.raw_selector==0 && o.raw_index==0 && o.raw_output==STATE_NEW);
        CHECK(o.selector==0 && o.index==0 && o.output==STATE_NEW);
        CHECK(o.caller==2 && o.lr==lr);
        CHECK(o.bytes_written==3 && o.publications==4);
        CHECK(r.get(STATE_NEW+STATE_ID_OFFSET,1)==2);
        CHECK(r.get(STATE_NEW+STATE_SCAN_OFFSET,1)==1);
        CHECK(r.get(STATE_NEW+STATE_ASPECT_OFFSET,1)==2);
        // The other thirteen bytes stay untouched poison.
        const auto after=read_state_at(r,STATE_NEW);
        for(size_t i=0;i<after.size();++i)
            if(i!=STATE_ID_OFFSET&&i!=STATE_SCAN_OFFSET&&i!=STATE_ASPECT_OFFSET)CHECK(after[i]==before[i]);
        CHECK(r.get(TOC_SLOT,8)==TOC);
        CHECK(r.since(from).size()==4);
        CHECK(r.control->video_state_calls==1 && r.control->video_state_returns==1);
        CHECK(r.execution.ppu.cpu.lr==lr && r.execution.ppu.cpu.cr==0x12345678);
        CHECK(r.execution.ppu.cpu.gpr[4]==0);
    }
}
void state_new_caller_rejects_wrong_output_and_unknown_lr() {
    // func_003AA898 serves only its own SP+0x7C record.
    {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);seed_state_at(r,STATE_NEW,0x5a);
        const auto before_state=read_state(r);
        const auto before_new=read_state_at(r,STATE_NEW);
        call_state(r,0,0,STATE,STATE_NEW_LR_FIRST);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::VideoStateStop && o.failure==StartupVideoStateFailure::WrongOutput);
        CHECK(!o.has_result && !o.toc_written && o.publications==0 && o.bytes_written==0 && o.caller==2);
        CHECK(read_state(r)==before_state && read_state_at(r,STATE_NEW)==before_new);
        CHECK(r.get(TOC_SLOT,8)==SENTINEL && !r.control->video_state_returns);
    }
    {
        Runtime r;configured(r);seed_toc(r);seed_state(r,0x5a);
        call_state(r,0,0,STATE,0x003aa000);
        const auto &o=r.control->video_state;
        CHECK(r.control->stop==StartupStop::VideoStateStop && o.failure==StartupVideoStateFailure::WrongLr);
        CHECK(!o.has_result && !o.toc_written && o.publications==0 && o.bytes_written==0 && o.caller==0);
        CHECK(r.get(STATE+STATE_ID_OFFSET,1)==0x5a && r.get(TOC_SLOT,8)==SENTINEL && !r.control->video_state_returns);
    }
}
void device_info_success_and_exact_effect() {
    Runtime r;configured(r);seed_toc(r);seed_info(r,0x5a);
    std::vector<unsigned char> before(0x1c8);CHECK(r.mem().read(r.mem().context,INFO,before.data(),before.size())==PS3_GUEST_MEMORY_OK);
    const auto from=r.published();call_device_info(r);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0 && r.execution.ppu.cpu.gpr[2]==TOC);
    const auto &o=r.control->video_device_info;
    CHECK(o.bridge_checked && o.topology==PresentationTopology::SingleNativeTarget && o.mode==NativePresentationMode::Fixed720p);
    CHECK(o.failure==StartupVideoBridgeFailure::None && o.has_result && o.toc_written && o.bytes_written==3 && o.publications==4);
    CHECK(r.get(INFO+INFO_COUNT,1)==1 && r.get(INFO+INFO_ID,1)==2 && r.get(INFO+INFO_ASPECT,1)==2);
    // The old wrong offsets must stay poison: this is the output-offset boundary check.
    CHECK(r.get(INFO+0x14,1)==0x5a && r.get(INFO+0x17,1)==0x5a);
    for(size_t i=0;i<before.size();++i)
        if(i!=INFO_COUNT&&i!=INFO_ID&&i!=INFO_ASPECT)CHECK(r.get(INFO+uint32_t(i),1)==before[i]);
    CHECK(r.get(TOC_SLOT,8)==TOC);
    const auto events=r.since(from);CHECK(events.size()==4);
    CHECK(r.control->video_device_info_calls==1 && r.control->video_device_info_returns==1);
    CHECK(r.execution.ppu.cpu.lr==0x00115980 && r.execution.ppu.cpu.cr==0x12345678);
    for(unsigned i=6;i<=10;++i)CHECK(r.execution.ppu.cpu.gpr[i]==UINT64_C(0xffffaaaa00000000)+i);
}
void device_info_rejects_unsupported_boundaries() {
    for(unsigned s=0;s<9;++s) {
        Runtime r;configured(r);seed_toc(r);seed_info(r,0x5a);
        switch(s) {
        case 0:r.execution.presentation_topology=PresentationTopology::Unconfigured;break;
        case 1:r.execution.presentation_topology=static_cast<PresentationTopology>(99);break;
        case 2:r.execution.presentation_mode=NativePresentationMode::Unconfigured;break;
        case 3:r.execution.presentation_mode=static_cast<NativePresentationMode>(99);break;
        case 4:break;
        case 5:break;
        case 6:break;
        case 7:break;
        case 8:break;
        }
        uint64_t selector=0,index=0,output=INFO,lr=0x00115980;
        if(s==4)lr=0x11597c; else if(s==5)selector=1; else if(s==6)index=1;
        else if(s==7)output=INFO+8; else if(s==8)output=UINT32_MAX;
        call_device_info(r,selector,index,output,lr);
        const auto &o=r.control->video_device_info;
        CHECK(r.control->stop==StartupStop::VideoDeviceInfoStop && !o.has_result && !o.toc_written && !o.publications && !o.bytes_written);
        CHECK(r.get(INFO+INFO_COUNT,1)==0x5a && r.get(INFO+INFO_ID,1)==0x5a && r.get(INFO+INFO_ASPECT,1)==0x5a && r.get(TOC_SLOT,8)==SENTINEL);
        CHECK(!r.control->video_device_info_returns && r.execution.ppu.cpu.gpr[3]==selector);
    }
}
void device_info_failure_prefix_and_cancellation() {
    // Each failure must preserve exactly the already committed prefix and never publish a result.
    for(unsigned s=0;s<9;++s) {
        Runtime r;configured(r);seed_toc(r);seed_info(r,0x5a);Probe p(r);
        switch(s) {
        case 0:p.fail_write=0;break;                  // first info store
        case 1:p.fail_write=1;break;                  // second info store
        case 2:p.fail_write=3;break;                  // TOC after three stores
        case 3:p.cancel_after_write=0;break;          // cancel after first store
        case 4:r.leave_trace_capacity(2);break;       // cannot reserve 4
        case 5:p.fail_read=0;break;                   // TOC preflight read
        case 6:r.control->cancelled.store(true);break;
        case 7:p.cancel_after_write=1;break;          // cancel after second store
        case 8:p.cancel_after_write=2;break;          // cancel after third store
        }
        call_device_info(r);p.restore();
        const auto &o=r.control->video_device_info;
        CHECK(!o.has_result && !r.control->video_device_info_returns);
        if(s==0)CHECK(r.get(INFO+INFO_COUNT,1)==0x5a&&r.get(INFO+INFO_ID,1)==0x5a&&r.get(INFO+INFO_ASPECT,1)==0x5a&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==1)CHECK(r.get(INFO+INFO_COUNT,1)==1&&r.get(INFO+INFO_ID,1)==0x5a&&r.get(INFO+INFO_ASPECT,1)==0x5a&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==2){CHECK(r.get(INFO+INFO_COUNT,1)==1&&r.get(INFO+INFO_ID,1)==2&&r.get(INFO+INFO_ASPECT,1)==2&&r.get(TOC_SLOT,8)==SENTINEL);}
        if(s==3)CHECK(r.control->stop==StartupStop::Cancelled&&r.get(INFO+INFO_COUNT,1)==1&&r.get(INFO+INFO_ID,1)==0x5a);
        if(s==4)CHECK(r.control->stop==StartupStop::TraceCapacity);
        if(s==5)CHECK(r.control->stop==StartupStop::InvalidMemory);
        if(s==6)CHECK(r.control->stop==StartupStop::Cancelled);
        if(s==7)CHECK(r.control->stop==StartupStop::Cancelled&&r.get(INFO+INFO_COUNT,1)==1&&r.get(INFO+INFO_ID,1)==2&&r.get(INFO+INFO_ASPECT,1)==0x5a&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==8)CHECK(r.control->stop==StartupStop::Cancelled&&r.get(INFO+INFO_COUNT,1)==1&&r.get(INFO+INFO_ID,1)==2&&r.get(INFO+INFO_ASPECT,1)==2&&r.get(TOC_SLOT,8)==SENTINEL);
    }
}
void availability_profile_and_values() {
    Runtime r;configured(r);
    struct Case{uint64_t id,aspect;int32_t expected;};
    const Case cases[]={{2,0,1},{2,2,1},{2,1,0},{1,0,0},{4,2,0},{UINT64_C(0x100000002),0,1},{2,UINT64_C(0x100000000),1}};
    for(const auto &c:cases) {
        seed_toc(r);const auto from=r.published();call_availability(r,0,c.id,c.aspect,UINT64_C(0xffffffffdeadbeef));
        const auto &o=r.control->video_availability;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==uint64_t(int64_t(c.expected)));
        CHECK(o.raw_selector==0 && o.raw_resolution_id==c.id && o.raw_aspect==c.aspect && o.raw_option==UINT64_C(0xffffffffdeadbeef));
        CHECK(o.resolution_id==uint32_t(c.id) && o.aspect==uint32_t(c.aspect) && o.available==c.expected);
        CHECK(o.toc_written && o.has_result && o.publications==1 && r.get(TOC_SLOT,8)==TOC && r.since(from).size()==1);
    }
    CHECK(r.control->video_availability_calls==sizeof(cases)/sizeof(cases[0]) && r.control->video_availability_returns==sizeof(cases)/sizeof(cases[0]));
}
void availability_rejects_profile_and_selector() {
    for(unsigned s=0;s<4;++s) {
        Runtime r;configured(r);seed_toc(r);
        if(s==0)r.execution.presentation_topology=PresentationTopology::Unconfigured;
        if(s==1)r.execution.presentation_mode=NativePresentationMode::Unconfigured;
        if(s==2)r.execution.presentation_mode=static_cast<NativePresentationMode>(99);
        const uint64_t selector=s==3?1:0;
        call_availability(r,selector,2,0);
        const auto &o=r.control->video_availability;
        CHECK(r.control->stop==StartupStop::VideoAvailabilityStop && !o.has_result && !o.toc_written && !o.publications);
        CHECK(r.get(TOC_SLOT,8)==SENTINEL && !r.control->video_availability_returns);
        if(s==3)CHECK(o.failure==StartupVideoBridgeFailure::WrongSelector);
    }
}
void availability_trace_and_memory_failure() {
    Runtime r;configured(r);seed_toc(r);r.leave_trace_capacity(0);
    call_availability(r,0,2,0);
    CHECK(r.control->stop==StartupStop::TraceCapacity&&!r.control->video_availability.has_result);
    Runtime q;configured(q);seed_toc(q);Probe p(q);p.fail_write=0;call_availability(q,0,2,0);p.restore();
    CHECK(q.control->stop==StartupStop::ProviderFailure&&!q.control->video_availability.has_result&&q.get(TOC_SLOT,8)==SENTINEL);
}
struct Row{uint8_t id;uint16_t w,h;};
constexpr Row TABLE[]={{1,1920,1080},{2,1280,720},{4,720,480},{5,720,576},{10,1600,1080},{11,1440,1080},
    {12,1280,1080},{13,960,1080},{100,1360,768},{129,1280,1470},{130,1920,1080},{131,1920,2205},
    {139,640,1470},{138,800,1470},{137,960,1470},{136,1024,1470},{145,1280,1470},{146,1920,1080},
    {155,640,1470},{154,800,1470},{153,960,1470},{152,1024,1470},{161,1920,1080}};
void resolution_all_rows() {
    Runtime r;seed_toc(r);
    for(const auto &row:TABLE) {
        const auto from=r.published();call_resolution(r,row.id,OUT);
        const auto &o=r.control->video_resolution;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
        CHECK(o.matched && o.width==row.w && o.height==row.h && o.writes==2 && o.publications==3 && o.toc_written && o.has_result);
        CHECK(r.get(OUT,2)==row.w && r.get(OUT+2,2)==row.h && r.get(TOC_SLOT,8)==TOC);
        CHECK(r.since(from).size()==3);
    }
    CHECK(r.control->video_resolution_calls==23 && r.control->video_resolution_returns==23);
}
void resolution_errors_and_pointer_admission() {
    {Runtime r;seed_toc(r);call_resolution(r,2,0);const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::None&&r.execution.ppu.cpu.gpr[3]==uint64_t(int64_t(int32_t(0x8002b222))));
     CHECK(o.failure==StartupVideoBridgeFailure::NullOutput&&!o.writes&&o.toc_written&&o.publications==1);}
    {Runtime r;seed_toc(r);call_resolution(r,3,OUT);const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::None&&r.execution.ppu.cpu.gpr[3]==uint64_t(int64_t(int32_t(0x8002b222))));
     CHECK(o.failure==StartupVideoBridgeFailure::UnknownResolution&&!o.writes&&o.toc_written);}
    {Runtime r;seed_toc(r);call_resolution(r,2,UINT64_C(0x100000000)+OUT);
     CHECK(r.control->stop==StartupStop::InvalidMemory&&!r.control->video_resolution.has_result);}
    {Runtime r;seed_toc(r);call_resolution(r,2,UINT32_MAX-1);
     CHECK(r.control->stop==StartupStop::InvalidMemory&&!r.control->video_resolution.has_result);}
}
void resolution_failure_prefix_and_cancel() {
    for(unsigned s=0;s<6;++s) {
        Runtime r;seed_toc(r);Probe p(r);
        switch(s){case 0:p.fail_write=0;break;case 1:p.fail_write=1;break;case 2:p.fail_write=2;break;
            case 3:p.cancel_after_write=0;break;case 4:p.cancel_after_write=1;break;case 5:p.fail_read=0;break;}
        call_resolution(r,2,OUT);p.restore();const auto &o=r.control->video_resolution;
        CHECK(!o.has_result&&!r.control->video_resolution_returns);
        if(s==0)CHECK(!o.height_written&&!o.width_written&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==1)CHECK(o.height_written&&!o.width_written&&r.get(OUT+2,2)==720&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==2)CHECK(o.height_written&&o.width_written&&r.get(OUT+2,2)==720&&r.get(OUT,2)==1280&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==3)CHECK(r.control->stop==StartupStop::Cancelled&&o.height_written&&!o.width_written&&r.get(OUT+2,2)==720);
        if(s==4)CHECK(r.control->stop==StartupStop::Cancelled&&o.height_written&&o.width_written&&r.get(OUT+2,2)==720&&r.get(OUT,2)==1280&&r.get(TOC_SLOT,8)==SENTINEL);
        if(s==5)CHECK(r.control->stop==StartupStop::InvalidMemory);
    }
}
void resolution_unknown_precedes_pointer_checks() {
    // After the full-64 NULL test, an unknown low32 id must return the SDK error
    // before any output-pointer validity or access. These pointers are all invalid,
    // so a pointer-first implementation stops instead of returning the error.
    const uint64_t outputs[]={UINT64_C(0x100000000)+OUT,UINT64_C(0x12345000),UINT64_MAX,OUT+UINT64_C(0x10000000)};
    for(const uint64_t output:outputs) {
        Runtime r;seed_toc(r);const auto from=r.published();call_resolution(r,3,output);
        const auto &o=r.control->video_resolution;
        CHECK(r.control->stop==StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3]==uint64_t(int64_t(int32_t(0x8002b222))));
        CHECK(o.failure==StartupVideoBridgeFailure::UnknownResolution&&o.has_result&&o.toc_written);
        CHECK(!o.writes&&o.publications==1&&r.get(TOC_SLOT,8)==TOC&&r.since(from).size()==1);
    }
}
void resolution_valid_id_split_scope_and_pointer_precedence() {
    // A valid id still refuses high32/overflow/unmapped output with no output access.
    const uint64_t invalid[]={UINT64_C(0x100000000)+OUT,UINT64_MAX,UINT64_C(0x12345000),UINT64_C(0xffffffff)};
    for(const uint64_t output:invalid) {
        Runtime r;seed_toc(r);call_resolution(r,2,output);
        const auto &o=r.control->video_resolution;
        CHECK(r.control->stop==StartupStop::InvalidMemory&&!o.has_result&&!o.writes);
        CHECK(r.get(TOC_SLOT,8)==SENTINEL&&!r.control->video_resolution_returns);
    }
    // Height is independently admitted at output+2; width at output may be outside
    // the writable scope. The committed height must survive the later native STOP.
    Runtime r;seed_toc(r);
    const unsigned char sentinel[4]{0x11,0x22,0x33,0x44};r.put(OUT,sentinel,sizeof sentinel);
    r.ranges[0]={STACK_BASE,0x600,true};
    r.ranges[1]={STACK_BASE+0x602,0x400,true};
    call_resolution(r,2,OUT);
    const auto &o=r.control->video_resolution;
    CHECK(r.control->stop==StartupStop::InvalidMemory&&!o.has_result&&!r.control->video_resolution_returns);
    CHECK(o.height_written&&!o.width_written&&o.writes==1&&o.publications==1);
    CHECK(r.get(OUT+2,2)==720&&r.get(OUT,2)==0x1122&&r.get(TOC_SLOT,8)==SENTINEL);
}
void resolution_sdk_error_cancellation_and_trace() {
    // Cancellation is polled before the SDK-error TOC publication too.
    {Runtime r;seed_toc(r);r.control->cancelled.store(true);call_resolution(r,3,OUT);const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::Cancelled&&!o.has_result&&!o.toc_written&&!o.publications&&!o.writes&&r.get(TOC_SLOT,8)==SENTINEL);}
    {Runtime r;seed_toc(r);Probe p(r);p.cancel_after_read=0;call_resolution(r,3,OUT);p.restore();const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::Cancelled&&!o.has_result&&!o.toc_written&&!o.publications&&!o.writes&&r.get(TOC_SLOT,8)==SENTINEL);}
    {Runtime r;seed_toc(r);Probe p(r);p.cancel_after_read=0;call_resolution(r,2,0);p.restore();const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::Cancelled&&!o.has_result&&!o.toc_written&&!o.publications&&!o.writes&&r.get(TOC_SLOT,8)==SENTINEL);}
    {Runtime r;seed_toc(r);Probe p(r);p.fail_write=0;call_resolution(r,3,OUT);p.restore();const auto &o=r.control->video_resolution;
     CHECK(r.control->stop==StartupStop::ProviderFailure&&!o.has_result&&!o.toc_written&&!o.writes&&r.get(TOC_SLOT,8)==SENTINEL);}
}
void presentation_mode_domain() {
    auto a=presentation_mode_outcome(NativePresentationMode::Fixed720p);
    CHECK(a.status==NativePresentationModeStatus::Configured&&a.width==1280&&a.height==720&&a.aspect_numerator==16&&a.aspect_denominator==9);
    CHECK(presentation_mode_outcome(NativePresentationMode::Unconfigured).status==NativePresentationModeStatus::Unconfigured);
    CHECK(presentation_mode_outcome(static_cast<NativePresentationMode>(99)).status==NativePresentationModeStatus::InvalidMode);
}
}
int main() {
    device_info_success_and_exact_effect();device_info_rejects_unsupported_boundaries();
    device_info_failure_prefix_and_cancellation();availability_profile_and_values();
    availability_rejects_profile_and_selector();availability_trace_and_memory_failure();
    resolution_all_rows();resolution_errors_and_pointer_admission();resolution_failure_prefix_and_cancel();
    resolution_unknown_precedes_pointer_checks();resolution_valid_id_split_scope_and_pointer_precedence();
    resolution_sdk_error_cancellation_and_trace();
    state_success_and_exact_effect();state_rejects_unsupported_boundaries();
    state_new_caller_success_and_exact_effect();
    state_new_caller_rejects_wrong_output_and_unknown_lr();
    state_admits_high32_only_selector_and_index_aliases();
    state_rejects_invalid_sp_and_overflow_before_effects();
    state_requires_whole_bound_record();state_failure_prefix_and_cancellation();
    presentation_mode_domain();
    std::printf("VIDEO_DISPLAY_BRIDGE_FAILURES=%d\n",failures);return failures?1:0;
}
