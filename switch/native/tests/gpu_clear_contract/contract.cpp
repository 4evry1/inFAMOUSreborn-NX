#include <deko3d.h>
#include "gpu_clear_deko.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
struct Memory { std::vector<unsigned char> bytes; };
static DkCopyBuf captured{};
static DkImageRect region{};
static std::vector<std::string> events;
struct Counts {unsigned waits, clears, copies, destroys, resets;};
static Counts* counts;
static unsigned failure_stage;
static bool queue_failure;
static DkGpuAddr readback_address;
static void record(const char* s) {
 events.emplace_back(s);
 if (!counts) return;
 const std::string event(s);
 if(event=="wait")++counts->waits;
 if(event=="clear")++counts->clears;
 if(event=="copy")++counts->copies;
 if(event=="destroy")++counts->destroys;
 if(event=="reset")++counts->resets;
}
extern "C" {
DkDevice dkDeviceCreate(const DkDeviceMaker*) {return reinterpret_cast<DkDevice>(1);}
void dkDeviceDestroy(DkDevice) {record("destroy");}
DkQueue dkQueueCreate(const DkQueueMaker*) {return reinterpret_cast<DkQueue>(2);}
void dkQueueDestroy(DkQueue) {record("destroy");}
bool dkQueueIsInErrorState(DkQueue) {return queue_failure && counts->waits==failure_stage;}
DkMemBlock dkMemBlockCreate(const DkMemBlockMaker* m) {return reinterpret_cast<DkMemBlock>(new Memory{std::vector<unsigned char>(m->size)});}
void dkMemBlockDestroy(DkMemBlock p) {record("destroy");delete reinterpret_cast<Memory*>(p);}
void* dkMemBlockGetCpuAddr(DkMemBlock p) {readback_address=reinterpret_cast<uintptr_t>(p);return reinterpret_cast<Memory*>(p)->bytes.data();}
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock p) {return reinterpret_cast<uintptr_t>(p);}
void dkImageLayoutInitialize(DkImageLayout*,const DkImageLayoutMaker*) {}
uint64_t dkImageLayoutGetSize(const DkImageLayout*) {return 262144;}
void dkImageInitialize(DkImage*,const DkImageLayout*,DkMemBlock,uint32_t) {}
DkCmdBuf dkCmdBufCreate(const DkCmdBufMaker*) {return reinterpret_cast<DkCmdBuf>(3);}
void dkCmdBufDestroy(DkCmdBuf) {record("destroy");}
void dkCmdBufAddMemory(DkCmdBuf,DkMemBlock,uint32_t,uint32_t) {}
void dkCmdBufBindRenderTargets(DkCmdBuf,const DkImageView* const*,uint32_t,const DkImageView*) {}
void dkCmdBufSetScissors(DkCmdBuf,uint32_t,const DkScissor*,uint32_t) {}
void dkCmdBufClearColor(DkCmdBuf,uint32_t,uint32_t,const void*) {record("clear");}
void dkCmdBufBarrier(DkCmdBuf,DkBarrier mode,uint32_t flags) {if(mode!=DkBarrier_Full || flags!=DkInvalidateFlags_L2Cache)std::abort();record("barrier");}
void dkCmdBufCopyImageToBuffer(DkCmdBuf,const DkImageView*,const DkImageRect* r,const DkCopyBuf* b,uint32_t flags) {if(flags || b->addr!=readback_address)std::abort();captured=*b;region=*r;record("copy");}
DkCmdList dkCmdBufFinishList(DkCmdBuf) {return 1;}
void dkCmdBufClear(DkCmdBuf) {record("reset");}
void dkQueueSubmitCommands(DkQueue,DkCmdList) {record("submit");}
void dkQueueSignalFence(DkQueue,DkFence*,bool flush) {if(!flush)std::abort();record("signal");}
void dkQueueFlush(DkQueue) {record("flush");}
DkResult dkFenceWait(DkFence*,int64_t timeout) {if(timeout!=5000000000LL)std::abort();record("wait");return !queue_failure && failure_stage && counts->waits==failure_stage ? DkResult_Timeout : DkResult_Success;}
}
int main() {
 struct rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
 for (auto dimensions: {std::pair<unsigned,unsigned>{64,32},{128,64},{256,256},{4,4}}) {
 events.clear(); captured={};region={};
 gpu_clear_request request{dimensions.first,dimensions.second,{0,0,0,1},0,0,4,4,{1,0,0,1}};
 std::vector<uint8_t> output(size_t(request.width)*request.height*4,0x39);
 if(gpu_clear_readback(request,output.data(),output.size())!=gpu_clear_status::success) return 2;
 if(region.width!=request.width || region.height!=request.height || region.depth!=1 || region.x || region.y || region.z)return 9;
 const size_t row_bytes=size_t(region.width)*4;
 const size_t pitch=captured.rowLength?captured.rowLength:row_bytes;
 const size_t rows=captured.imageHeight?captured.imageHeight:region.height;
 const size_t end=(region.height-1)*pitch+row_bytes;
 if(pitch<row_bytes || rows<region.height || end!=output.size()) {
 std::fprintf(stderr,"FAIL actual copy ABI width=%u height=%u rowLength=%u imageHeight=%u effective_pitch=%zu row_bytes=%zu end=%zu expected=%zu\n",request.width,request.height,captured.rowLength,captured.imageHeight,pitch,row_bytes,end,output.size());return 1; }
 // The spy never draws or copies pixels; the sentinel must remain unmodified.
 if(!std::all_of(output.begin(),output.end(),[](uint8_t b){return b==0xa5;}))return 3;
 const std::vector<std::string> expected{
 "signal","flush","wait","clear","clear","submit","signal","flush","wait",
 "reset","barrier","copy","submit","signal","flush","wait",
 "destroy","destroy","destroy","destroy","destroy","destroy"};
 if(events!=expected) {std::fputs("FAIL stage command/fence ordering\n",stderr);return 4;}
 }
 counts=static_cast<Counts*>(mmap(nullptr,sizeof(Counts),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
 auto* output=static_cast<uint8_t*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
 if(counts==MAP_FAILED || output==MAP_FAILED)return 5;
 for(bool error:{false,true})for(unsigned stage=1;stage<=3;++stage) {
 *counts={};std::fill(output,output+8192,0x39);failure_stage=stage;queue_failure=error;
 std::fflush(nullptr);
 pid_t child=fork();if(child<0)return 6;
 if(!child) {gpu_clear_request request{64,32,{0,0,0,1},0,0,4,4,{1,0,0,1}};gpu_clear_readback(request,output,8192);_exit(99);}
 int status=0;if(waitpid(child,&status,0)!=child)return 7;
 const unsigned expected_clears=stage==1?0:2;
 const unsigned expected_copies=stage==3?1:0;
 if(!WIFSIGNALED(status)||WTERMSIG(status)!=SIGABRT||counts->destroys||counts->waits!=stage||counts->clears!=expected_clears||counts->copies!=expected_copies||counts->resets!=(stage==3?1u:0u)||!std::all_of(output,output+8192,[](uint8_t b){return b==0x39;})) {
 std::fprintf(stderr,"FAIL stage=%u queue_error=%d status=%d waits=%u clears=%u copies=%u destroys=%u resets=%u\n",stage,error,status,counts->waits,counts->clears,counts->copies,counts->destroys,counts->resets);return 8;}
 }
 munmap(output,8192);munmap(counts,sizeof(Counts));
 std::puts("PASS actual adapter: copy ABI at four sizes; stage ordering; six wait/queue failures abort without cleanup/readback. CPU ABI spy only; no GPU/pixel proof.");
}
