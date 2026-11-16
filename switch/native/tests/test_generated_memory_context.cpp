#include "ppu_memory_context.h"
extern "C" {
#include "guest_space_host.h"
}
#include <cassert>
#include <cstring>
#include <pthread.h>
#include <thread>

void func_F0000000(ppu_context *);
void func_F0000001(ppu_context *);
void func_F0000002(ppu_context *);
void func_F0000003(ppu_context *);
void func_F0000004(ppu_context *);
void func_F0000005(ppu_context *);
void func_F0000006(ppu_context *);
void func_F0000007(ppu_context *);
void func_F0000008(ppu_context *);
void func_F0000009(ppu_context *);
void func_F000000A(ppu_context *);
void func_F000000B(ppu_context *);
void func_F000000C(ppu_context *);
void func_F000000D(ppu_context *);

namespace {
struct Stopped { PpuMemoryFailure failure; };
struct Boundary { bool cancelled = false; pthread_mutex_t *mutex = nullptr; };
[[noreturn]] void stop(void *context, PpuMemoryFailure failure) {
    auto *boundary=static_cast<Boundary *>(context);
    if(boundary && boundary->mutex) {
        assert(!pthread_mutex_trylock(boundary->mutex));
        assert(!pthread_mutex_unlock(boundary->mutex));
    }
    throw Stopped{failure};
}
bool cancelled(void *context) { return context && static_cast<Boundary *>(context)->cancelled; }
struct Pages {
    static constexpr uint32_t base = 0x10000;
    static constexpr size_t size = 8192;
    ElfTestHostSpace *space = elf_test_host_space_create(base+size);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    bool fail_read = false, fail_write = false;
    unsigned reads = 0, writes = 0;
    ~Pages() { elf_test_host_space_destroy(space); }
    unsigned char byte(size_t offset) {
        unsigned char result;
        assert(backing.read(backing.context,base+offset,&result,1)==PS3_GUEST_MEMORY_OK);
        return result;
    }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<Pages *>(p);
        return s.backing.commit(s.backing.context,a,n);
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
        auto &s=*static_cast<Pages *>(p);
        return s.backing.decommit(s.backing.context,a,n);
    }
    static ps3_guest_memory_status read(void *p,uint32_t a,void *d,size_t n) {
        auto &s=*static_cast<Pages *>(p); ++s.reads;
        if (s.fail_read) { if(n) static_cast<unsigned char *>(d)[0]=0xee; return PS3_GUEST_MEMORY_PLATFORM_FAILURE; }
        return s.backing.read(s.backing.context,a,d,n);
    }
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *d,size_t n) {
        auto &s=*static_cast<Pages *>(p); ++s.writes;
        if(s.fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        return s.backing.write(s.backing.context,a,d,n);
    }
    static ps3_guest_memory_status zero(void *p,uint32_t a,uint64_t n) {
        auto &s=*static_cast<Pages *>(p); ++s.writes;
        if(s.fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        return s.backing.zero(s.backing.context,a,n);
    }
    ps3_guest_memory port() { return {this,commit,decommit,read,write,zero}; }
};
void lock(void *p) { assert(!pthread_mutex_lock(static_cast<pthread_mutex_t *>(p))); }
void unlock(void *p) { assert(!pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p))); }
struct Runtime {
    Pages pages;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record records[2]{};
    ps3_atomic_pin_record pins[2]{};
    ps3_guest_memory memory{};
    Ps3PpuExecutionServices services{};
    PpuMemoryRamRange ram{Pages::base,Pages::size,true};
    PpuMemoryBinding binding;
    Boundary boundary{false,&mutex};
    ps3_atomic_domain &initialize_domain() {
        assert(pages.space);
        assert(ps3_atomic_domain_init(&domain,pages.port(),{&mutex,lock,unlock},records,2,pins,2)==PS3_GUEST_MEMORY_OK);
        return domain;
    }
    Runtime():binding(initialize_domain(),&ram,1) {
        memory=ps3_atomic_domain_memory(&domain); services.memory=&memory;
        assert(ps3_atomic_commit(&domain,Pages::base,Pages::size)==PS3_GUEST_MEMORY_OK);
    }
    ~Runtime() { pthread_mutex_destroy(&mutex); }
};
void admit(PpuMemoryExecution &owner,Runtime &runtime) {
    assert(ppu_memory_admit(owner,runtime.binding,runtime.services,{&runtime.boundary,cancelled,stop})==PpuMemoryAdmission::Ready);
}
template<class F> void fails(F operation,PpuMemoryStop reason) {
    try { operation(); assert(false); } catch(const Stopped &s) { assert(s.failure.reason==reason); }
}
void run_contracts() {
    Runtime a,b; PpuMemoryExecution x,y,z; admit(x,a); admit(y,b);
    ppu_native_write32(&x.cpu,0x100010001ULL,0x12345678);
    assert(ppu_native_read32(&x.cpu,0x10001)==0x12345678);
    assert(ppu_native_read32(&y.cpu,0x10001)==0);
    assert(a.pages.byte(1)==0x12 && a.pages.byte(4)==0x78);
    ppu_native_write64(&x.cpu,0x10007,0x0123456789abcdefULL);
    assert(ppu_native_read64(&x.cpu,0x10007)==0x0123456789abcdefULL);
    unsigned reads=a.pages.reads;
    fails([&]{ppu_native_read64(&x.cpu,0xffffffff);},PpuMemoryStop::InvalidRange);
    assert(a.pages.reads==reads);
    ppu_native_read_bytes(&x.cpu,0,nullptr,0); ppu_native_write_bytes(&x.cpu,0,nullptr,0);
    assert(a.pages.reads==reads);
    unsigned char byte_source[24], byte_destination[24];
    std::memset(byte_source,0xa7,sizeof(byte_source));
    const size_t valid_byte_lengths[]={1,16};
    for(size_t length : valid_byte_lengths) {
        ppu_native_write_bytes(&x.cpu,0x10100,byte_source,length);
        std::memset(byte_destination,0x55,sizeof(byte_destination));
        ppu_native_read_bytes(&x.cpu,0x10100,byte_destination,length);
        assert(!std::memcmp(byte_source,byte_destination,length));
        for(size_t index=length;index<sizeof(byte_destination);++index) assert(byte_destination[index]==0x55);
    }
    const size_t invalid_byte_lengths[]={17,23,24};
    for(size_t length : invalid_byte_lengths) {
        const unsigned before_reads=a.pages.reads, before_writes=a.pages.writes;
        std::memset(byte_destination,0x55,sizeof(byte_destination));
        fails([&]{ppu_native_read_bytes(&x.cpu,0x10100,byte_destination,length);},PpuMemoryStop::InvalidRange);
        fails([&]{ppu_native_write_bytes(&x.cpu,0x10100,byte_source,length);},PpuMemoryStop::InvalidRange);
        assert(a.pages.reads==before_reads && a.pages.writes==before_writes);
        for(auto byte:byte_destination) assert(byte==0x55);
    }
    unsigned char destination[16]; std::memset(destination,0x55,16);
    a.pages.fail_read=true;
    fails([&]{ppu_native_read_bytes(&x.cpu,0x10000,destination,16);},PpuMemoryStop::ProviderFailure);
    for(auto byte:destination) assert(byte==0x55);
    a.pages.fail_read=false;
    const auto original_cpu=x.cpu;
    a.pages.fail_read=true;
    fails([&]{x.cpu.gpr[2]=ppu_native_read64(&x.cpu,0x10000);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&x.cpu,&original_cpu,sizeof(original_cpu)));
    a.pages.fail_read=false;
    admit(z,a);
    ppu_native_write32(&x.cpu,0x10000,7);
    assert(ppu_native_load_reserve32(&x.cpu,0x10000)==7);
    std::thread writer([&]{ppu_native_write32(&z.cpu,0x10000,7);}); writer.join();
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    ppu_native_write32(&z.cpu,0x10000,9); ppu_native_write32(&z.cpu,0x10000,7);
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    ppu_native_write32(&z.cpu,0x10080,9);
    assert(ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    assert(!ppu_native_store_conditional64(&x.cpu,0x10000,8));
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    a.boundary.cancelled=true; reads=a.pages.reads;
    fails([&]{ppu_native_store_conditional32(&x.cpu,0x10000,8);},PpuMemoryStop::Cancelled);
    assert(a.pages.reads==reads);
    a.boundary.cancelled=false;
    assert(ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    assert(!ppu_native_store_conditional32(&x.cpu,0x10080,8));
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    fails([&]{ppu_native_store_conditional32(&x.cpu,0x20000,8);},PpuMemoryStop::UnsupportedRegion);
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    a.pages.fail_write=true;
    fails([&]{ppu_native_store_conditional32(&x.cpu,0x10000,8);},PpuMemoryStop::ProviderFailure);
    a.pages.fail_write=false;
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ppu_native_load_reserve32(&x.cpu,0x10000);
    assert(ps3_atomic_decommit(&a.domain,0x10000,4096)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_commit(&a.domain,0x10000,4096)==PS3_GUEST_MEMORY_OK);
    assert(!ppu_native_store_conditional32(&x.cpu,0x10000,8));
    ps3_atomic_pin pin{};
    assert(ps3_atomic_pin_acquire(&a.domain,0x10000,4096,&pin)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_decommit(&a.domain,0x10000,4096)==PS3_GUEST_MEMORY_BUSY);
    assert(ps3_atomic_pin_release(&a.domain,&pin)==PS3_GUEST_MEMORY_OK);
    unsigned char before[16],replacement[16];
    std::memset(replacement,0x77,sizeof(replacement));
    assert(a.memory.read(a.memory.context,0x10ff0,before,sizeof(before))==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_decommit(&a.domain,0x11000,4096)==PS3_GUEST_MEMORY_OK);
    fails([&]{ppu_native_write_bytes(&x.cpu,0x10ff8,replacement,16);},PpuMemoryStop::ProviderFailure);
    unsigned char after[16];
    assert(a.memory.read(a.memory.context,0x10ff0,after,sizeof(after))==PS3_GUEST_MEMORY_OK);
    assert(!std::memcmp(before,after,sizeof(before)));
    assert(ps3_atomic_commit(&a.domain,0x11000,4096)==PS3_GUEST_MEMORY_OK);
    ppu_native_write16(&x.cpu,0x10000,65535);
    assert(ppu_native_atomic_add16(&x.cpu,0x10000,2)==1);
    ppu_native_zero128(&x.cpu,0x10000);
    for(size_t i=0;i<128;++i) assert(a.pages.byte(i)==0);
    PpuMemoryExecution exhausted;
    assert(ppu_memory_admit(exhausted,a.binding,b.services,{nullptr,cancelled,stop})==PpuMemoryAdmission::InvalidBinding);
    assert(ppu_memory_admit(exhausted,a.binding,a.services,{nullptr,cancelled,nullptr})==PpuMemoryAdmission::InvalidBinding);
    assert(ppu_memory_admit(exhausted,a.binding,a.services,{nullptr,cancelled,stop})==PpuMemoryAdmission::OwnerUnavailable);
    ps3_atomic_owner stale=x.reservation;
    assert(ppu_memory_release_after_join(x)==PS3_GUEST_MEMORY_OK);
    admit(exhausted,a);
    uint32_t value;
    assert(ps3_atomic_load_reserve32(&a.domain,stale,0x10000,&value)!=PS3_GUEST_MEMORY_OK);
    assert(ppu_memory_release_after_join(exhausted)==PS3_GUEST_MEMORY_OK);
    assert(ppu_memory_release_after_join(z)==PS3_GUEST_MEMORY_OK);
    assert(ppu_memory_release_after_join(y)==PS3_GUEST_MEMORY_OK);
    PpuMemoryRamRange limited_ram{Pages::base,4096};
    assert(!limited_ram.writable);
    PpuMemoryBinding limited(a.domain,&limited_ram,1);
    PpuMemoryExecution special;
    assert(ppu_memory_admit(special,limited,a.services,{&a.boundary,cancelled,stop})==PpuMemoryAdmission::Ready);
    reads=a.pages.reads;
    fails([&]{ppu_native_read32(&special.cpu,0x11000);},PpuMemoryStop::UnsupportedRegion);
    assert(a.pages.reads==reads);
    ppu_native_read32(&special.cpu,Pages::base);
    unsigned writes=a.pages.writes;
    fails([&]{ppu_native_write32(&special.cpu,Pages::base,123);},PpuMemoryStop::PermissionDenied);
    assert(a.pages.writes==writes);
    ppu_native_load_reserve32(&special.cpu,Pages::base);
    fails([&]{ppu_native_store_conditional32(&special.cpu,Pages::base,123);},PpuMemoryStop::PermissionDenied);
    assert(a.pages.writes==writes);
    int stored=-1;
    assert(ps3_atomic_store_conditional32(&a.domain,special.reservation,Pages::base,123,&stored)==PS3_GUEST_MEMORY_OK);
    assert(!stored);
    assert(ppu_memory_release_after_join(special)==PS3_GUEST_MEMORY_OK);
    PpuMemoryRamRange overlapping_ram[]={{Pages::base,Pages::size,true},{Pages::base+4096,4096,false}};
    PpuMemoryBinding overlapping(a.domain,overlapping_ram,2);
    PpuMemoryExecution ambiguous;
    assert(ppu_memory_admit(ambiguous,overlapping,a.services,{nullptr,cancelled,stop})==PpuMemoryAdmission::InvalidBinding);
}
bool sc_succeeded(const ppu_context &cpu) { return (cpu.cr&0xf0000000u)==0x20000000u; }
void run_derived_contracts() {
    Runtime runtime; PpuMemoryExecution reader,writer; admit(reader,runtime); admit(writer,runtime);
    reader.cpu.gpr[4]=0x100010001ULL; writer.cpu.gpr[4]=0x10001;
    writer.cpu.gpr[3]=0x12345678;
    func_F0000001(&writer.cpu); func_F0000000(&reader.cpu);
    assert(reader.cpu.gpr[3]==0x12345678);
    reader.cpu.gpr[4]=writer.cpu.gpr[4]=Pages::base;
    unsigned char vector[16];
    for(size_t i=0;i<16;++i) vector[i]=static_cast<unsigned char>(i*11+3);
    assert(runtime.memory.write(runtime.memory.context,Pages::base,vector,16)==PS3_GUEST_MEMORY_OK);
    func_F0000004(&reader.cpu);
    assert(!std::memcmp(&reader.cpu.vr[2],vector,16));
    func_F0000006(&reader.cpu);
    assert(reader.cpu.gpr[7]==(uint32_t(vector[0])|uint32_t(vector[1])<<8|uint32_t(vector[2])<<16|uint32_t(vector[3])<<24));
    for(unsigned lane=0;lane<16;++lane) {
        reader.cpu.gpr[4]=Pages::base+lane;
        func_F000000C(&reader.cpu);
        unsigned char expected[16]{};
        std::memcpy(expected+(lane&~3u),vector+(lane&~3u),4);
        assert(!std::memcmp(&reader.cpu.vr[2],expected,16));
        func_F000000D(&reader.cpu);
        for(unsigned i=0;i<16;++i) expected[i]=lane+i<16?vector[lane+i]:0;
        assert(!std::memcmp(&reader.cpu.vr[2],expected,16));
    }
    reader.cpu.gpr[4]=Pages::base;
    const auto original_cpu=reader.cpu;
    runtime.pages.fail_read=true;
    fails([&]{func_F0000004(&reader.cpu);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&reader.cpu,&original_cpu,sizeof(original_cpu)));
    fails([&]{func_F0000002(&reader.cpu);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&reader.cpu,&original_cpu,sizeof(original_cpu)));
    fails([&]{func_F000000C(&reader.cpu);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&reader.cpu,&original_cpu,sizeof(original_cpu)));
    fails([&]{func_F000000D(&reader.cpu);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&reader.cpu,&original_cpu,sizeof(original_cpu)));
    runtime.pages.fail_read=false;
    std::memcpy(&writer.cpu.vr[2],vector,16);
    using GeneratedWriter=void (*)(ppu_context *);
    GeneratedWriter writers[]={func_F0000001,func_F0000005,func_F0000007,func_F0000008,func_F0000009};
    for(auto generated_writer:writers) {
        func_F0000002(&reader.cpu);
        std::thread competing_writer([&]{generated_writer(&writer.cpu);}); competing_writer.join();
        func_F0000003(&reader.cpu);
        assert(!sc_succeeded(reader.cpu));
    }
    writer.cpu.gpr[3]=ppu_native_read32(&writer.cpu,Pages::base);
    func_F0000002(&reader.cpu);
    std::thread same_value_writer([&]{func_F0000001(&writer.cpu);}); same_value_writer.join();
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    writer.cpu.gpr[3]^=1; func_F0000001(&writer.cpu);
    writer.cpu.gpr[3]^=1; func_F0000001(&writer.cpu);
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    writer.cpu.gpr[4]=Pages::base+64; func_F0000001(&writer.cpu);
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    writer.cpu.gpr[4]=Pages::base+128; func_F0000001(&writer.cpu);
    func_F0000003(&reader.cpu); assert(sc_succeeded(reader.cpu));
    writer.cpu.gpr[4]=Pages::base;
    func_F0000002(&reader.cpu);
    assert(runtime.services.memory->write(runtime.services.memory->context,Pages::base,vector,16)==PS3_GUEST_MEMORY_OK);
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    const auto before_failed_sc=reader.cpu;
    runtime.pages.fail_write=true;
    fails([&]{func_F0000003(&reader.cpu);},PpuMemoryStop::ProviderFailure);
    assert(!std::memcmp(&reader.cpu,&before_failed_sc,sizeof(before_failed_sc)));
    runtime.pages.fail_write=false;
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    assert(ps3_atomic_decommit(&runtime.domain,Pages::base,4096)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_commit(&runtime.domain,Pages::base,4096)==PS3_GUEST_MEMORY_OK);
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    reader.cpu.reserve_addr=0; reader.cpu.reserve_value=0;
    func_F0000003(&reader.cpu);
    assert(sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    reader.cpu.gpr[4]=Pages::base+128;
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    reader.cpu.gpr[4]=Pages::base;
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000002(&reader.cpu);
    func_F000000B(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    func_F0000003(&reader.cpu); assert(!sc_succeeded(reader.cpu));
    ppu_native_write64(&reader.cpu,Pages::base,0x123456789abcdef0ULL);
    func_F000000A(&reader.cpu); assert(reader.cpu.gpr[6]==0x123456789abcdef0ULL);
    func_F000000B(&reader.cpu); assert(sc_succeeded(reader.cpu));
    ppu_native_write16(&writer.cpu,Pages::base,65535);
    func_F0000009(&writer.cpu); assert(writer.cpu.gpr[8]==0);
    func_F0000008(&writer.cpu);
    for(size_t i=0;i<128;++i) assert(runtime.pages.byte(i)==0);
    assert(ppu_memory_release_after_join(reader)==PS3_GUEST_MEMORY_OK);
    assert(ppu_memory_release_after_join(writer)==PS3_GUEST_MEMORY_OK);
}
}
int main() {
    run_contracts();
    run_derived_contracts();
}
