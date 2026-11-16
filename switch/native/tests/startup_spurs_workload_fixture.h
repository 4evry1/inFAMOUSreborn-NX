#pragma once

// Include after the real startup attach fixture, which owns Runtime and its memory.
namespace {
struct IdleServices {
    ps3_pthread_guard guards[PS3_SPURS_MAX_INSTANCES]{};
    std::atomic<unsigned> execution_calls{0};
    IdleServices() { for(auto &guard:guards) CHECK(!ps3_pthread_guard_init(&guard)); }
    ~IdleServices() { for(auto &guard:guards) ps3_pthread_guard_destroy(&guard); }
    static ps3_spurs_policy_execution_result refuse(void *context,const ps3_spurs_policy_execution_request *) {
        ++static_cast<IdleServices *>(context)->execution_calls;
        ps3_spurs_policy_execution_result result{};result.status=PS3_SPURS_POLICY_UNSUPPORTED;return result;
    }
    static ps3_spurs_policy_execution_result step(void *context,uint32_t,uint64_t) { return refuse(context,nullptr); }
    static ps3_spurs_policy_execution_status release(void *,uint32_t) { return PS3_SPURS_POLICY_UNSUPPORTED; }
    void install(Runtime &runtime) {
        ps3_instance_guard guard_ports[PS3_SPURS_MAX_INSTANCES];
        ps3_spurs_policy_execution_port ports[PS3_SPURS_MAX_INSTANCES];
        for(unsigned i=0;i<PS3_SPURS_MAX_INSTANCES;++i) {
            guard_ports[i]=ps3_pthread_guard_port(&guards[i]); ports[i]={this,refuse,step,step,release,release};
        }
        CHECK(!ps3_spurs_runtime_set_workload_services(&runtime.comp->runtime,guard_ports,ports));
    }
};
void register_idle_workload(Runtime &r, unsigned char priority = 2) {
    unsigned char image[56]{};
    const auto put=[&](unsigned offset,uint32_t value) { for(unsigned i=0;i<4;++i) image[offset+i]=uint8_t(value>>(24-i*8)); };
    put(0,1);put(4,0x220002);put(8,0x69c880);put(12,0x2d80);put(20,0x404e9300);
    for(unsigned i=0;i<5;++i) image[24+i]=priority;
    put(32,1);put(36,5);r.put(ATTR,image,sizeof image);
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=INSTANCE1;cpu.gpr[4]=PORT_EA;cpu.gpr[5]=ATTR;
    r.run(0xc0158d8b,false);CHECK(r.control->stop==StartupStop::None && cpu.gpr[3]==0 && r.get(PORT_EA,4)==0);
}
}
