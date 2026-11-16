#include "user_memory_service.h"

static int valid_service(const ps3_user_memory_service *service) {
    return service && service->allocator && service->domain;
}
static void encode_be32(unsigned char *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (24 - i * 8));
}
static ps3_user_memory_service_result release_output(
    ps3_user_memory_service *service, ps3_atomic_pin pin,
    ps3_user_memory_service_result result) {
    ps3_guest_memory_status status = ps3_atomic_pin_release(service->domain, &pin);
    result.pin_release_status = status;
    if (status != PS3_GUEST_MEMORY_OK) {
        result.retained_pin = 1;
        result.pin = pin;
        if (result.status == PS3_USER_MEMORY_SERVICE_OK) {
            result.status = PS3_USER_MEMORY_SERVICE_PIN_RELEASE_FAILURE;
            result.memory_status = status;
        }
    }
    return result;
}
ps3_user_memory_service_result ps3_user_memory_allocate_and_publish_be32(
    ps3_user_memory_service *service, uint64_t size, uint64_t alignment, uint64_t output) {
    ps3_user_memory_service_result result = {0};
    if (!valid_service(service)) {
        result.status = PS3_USER_MEMORY_SERVICE_ARGUMENT;
        return result;
    }
    ps3_atomic_pin pin;
    result.memory_status = ps3_atomic_pin_acquire(service->domain, output, 4, &pin);
    if (result.memory_status != PS3_GUEST_MEMORY_OK) {
        result.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
        return result;
    }
    uint32_t address;
    result.allocator = ps3_user_memory_allocate(service->allocator, size, alignment, &address);
    if (result.allocator.status != PS3_USER_MEMORY_OK) {
        result.status = PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE;
    } else {
        unsigned char bytes[4];
        encode_be32(bytes, address);
        result.memory_status = ps3_atomic_write(service->domain, output, bytes, sizeof bytes);
        result.published = result.memory_status == PS3_GUEST_MEMORY_OK;
        if (result.memory_status != PS3_GUEST_MEMORY_OK) {
            result.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
            result.rollback = ps3_user_memory_free(service->allocator, address);
            if (result.rollback.status != PS3_USER_MEMORY_OK) {
                result.status = PS3_USER_MEMORY_SERVICE_ROLLBACK_FAILURE;
                result.retained_allocation = 1;
                result.retained_address = address;
            }
        }
    }
    return release_output(service, pin, result);
}
ps3_user_memory_service_result ps3_user_memory_free_owned(
    ps3_user_memory_service *service, uint32_t address) {
    ps3_user_memory_service_result result = {0};
    if (!valid_service(service)) {
        result.status = PS3_USER_MEMORY_SERVICE_ARGUMENT;
        return result;
    }
    result.allocator = ps3_user_memory_free(service->allocator, address);
    if (result.allocator.status != PS3_USER_MEMORY_OK) {
        result.status = PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE;
        if (result.allocator.status == PS3_USER_MEMORY_MEMORY_FAILURE) {
            result.retained_allocation = 1;
            result.retained_address = address;
        }
    }
    return result;
}
ps3_user_memory_service_result ps3_user_memory_query_and_publish_be32(
    ps3_user_memory_service *service, uint64_t output) {
    ps3_user_memory_service_result result = {0};
    if (!valid_service(service)) {
        result.status = PS3_USER_MEMORY_SERVICE_ARGUMENT;
        return result;
    }
    ps3_atomic_pin pin;
    result.memory_status = ps3_atomic_pin_acquire(service->domain, output, 8, &pin);
    if (result.memory_status != PS3_GUEST_MEMORY_OK) {
        result.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
        return result;
    }
    ps3_user_memory_info info;
    result.allocator.status = ps3_user_memory_query(service->allocator, &info);
    if (result.allocator.status != PS3_USER_MEMORY_OK) {
        result.status = PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE;
    } else if (info.quota_bytes > UINT32_MAX || info.available_bytes > UINT32_MAX) {
        result.status = PS3_USER_MEMORY_SERVICE_OVERFLOW;
    } else {
        unsigned char bytes[8];
        encode_be32(bytes, (uint32_t)info.quota_bytes);
        encode_be32(bytes + 4, (uint32_t)info.available_bytes);
        result.memory_status = ps3_atomic_write(service->domain, output, bytes, sizeof bytes);
        result.published = result.memory_status == PS3_GUEST_MEMORY_OK;
        if (result.memory_status != PS3_GUEST_MEMORY_OK)
            result.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
    }
    return release_output(service, pin, result);
}
