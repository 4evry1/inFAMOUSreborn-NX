#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PS3_NATIVE_PROBE_VERSION
#error Dedicated GPU diagnostic requires an explicit version
#endif

static const size_t GPU_DIAGNOSTIC_HEAP_BYTES = 64u * 1024u * 1024u;
size_t __nx_heap_size = 64u * 1024u * 1024u;
int native_test_gpu_clear(int corrupt_expected);

static void debug_line(const char *line)
{
    svcOutputDebugString(line, strlen(line));
}

static void log_launch_memory(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    const uintptr_t heap_start = (uintptr_t)fake_heap_start;
    const uintptr_t heap_end = (uintptr_t)fake_heap_end;
    u64 heap_base = 0, resource_total = 0, resource_used = 0;
    MemoryInfo heap_info = {0};
    u32 page_info = 0;
    Result heap_result = svcGetInfo(&heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(heap_result)) heap_result = svcQueryMemory(&heap_info, &page_info, heap_base);
    Result total_result = svcGetInfo(&resource_total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    Result used_result = svcGetInfo(&resource_used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
    printf("LAUNCH_MEMORY requested_heap_bytes=0x%llx actual_allocator_heap_bytes=0x%llx mapped_heap_bytes=0x%llx heap_result=0x%x nso=%d heap_override=%d applet_type=%d\n",
           (unsigned long long)GPU_DIAGNOSTIC_HEAP_BYTES,
           (unsigned long long)(heap_end >= heap_start ? heap_end - heap_start : 0),
           (unsigned long long)heap_info.size, heap_result, envIsNso(), envHasHeapOverride(), appletGetAppletType());
    printf("SYSTEM_RESOURCE total=0x%llx total_result=0x%x used=0x%llx used_result=0x%x\n",
           (unsigned long long)resource_total, total_result,
           (unsigned long long)resource_used, used_result);
}

int main(void)
{
    const char *directory = "sdmc:/switch/infamous-native-gpu";
    if ((mkdir("sdmc:/switch", 0777) != 0 && errno != EEXIST) ||
        (mkdir(directory, 0777) != 0 && errno != EEXIST)) {
        debug_line("INFAMOUS_NATIVE_GPU ERROR log-directory\n");
        return 2;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/gpu-clear-" PS3_NATIVE_PROBE_VERSION "-%llu.log",
             directory, (unsigned long long)armGetSystemTick());
    int descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (descriptor < 0) {
        debug_line("INFAMOUS_NATIVE_GPU ERROR log-create\n");
        return 2;
    }
    if (dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0) {
        close(descriptor);
        debug_line("INFAMOUS_NATIVE_GPU ERROR log-redirect\n");
        return 2;
    }
    close(descriptor);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("inFAMOUS native GPU diagnostic " PS3_NATIVE_PROBE_VERSION "\n");
    printf("COMPOSITION dedicated-gpu-only positive-clear shared_flags=false\n");
    log_launch_memory();
    fflush(stdout);
    fsync(STDOUT_FILENO);
    debug_line("INFAMOUS_NATIVE_GPU START " PS3_NATIVE_PROBE_VERSION "\n");
    const int failed = native_test_gpu_clear(0) != 0;
    printf("RESULT %s failed_suites=%d\n", failed ? "FAIL" : "PASS", failed);
    fflush(stdout);
    fsync(STDOUT_FILENO);
    debug_line(failed ? "INFAMOUS_NATIVE_GPU FAIL\n" : "INFAMOUS_NATIVE_GPU PASS\n");
    return failed;
}
