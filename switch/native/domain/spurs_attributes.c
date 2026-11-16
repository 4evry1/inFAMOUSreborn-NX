#include "spurs_attributes.h"
#include <string.h>

static void encode_be32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

ps3_spurs_attribute_status ps3_spurs_attribute_validate(uint32_t address)
{
    if (!address) return PS3_SPURS_ATTRIBUTE_NULL;
    if (address % PS3_SPURS_ATTRIBUTE_ALIGNMENT) return PS3_SPURS_ATTRIBUTE_ALIGN;
    return PS3_SPURS_ATTRIBUTE_OK;
}

ps3_spurs_attribute_status ps3_spurs_attribute_validate_prefix(
    uint32_t address, uint32_t prefix, uint32_t size)
{
    if (!address || !prefix) return PS3_SPURS_ATTRIBUTE_NULL;
    const ps3_spurs_attribute_status status = ps3_spurs_attribute_validate(address);
    if (status != PS3_SPURS_ATTRIBUTE_OK) return status;
    if (size > PS3_SPURS_ATTRIBUTE_PREFIX_CAPACITY) return PS3_SPURS_ATTRIBUTE_INVAL;
    return PS3_SPURS_ATTRIBUTE_OK;
}

ps3_spurs_attribute_status ps3_spurs_attribute_validate_group_type(
    uint32_t address, int32_t type)
{
    const ps3_spurs_attribute_status status = ps3_spurs_attribute_validate(address);
    if (status != PS3_SPURS_ATTRIBUTE_OK) return status;
    if (type != PS3_SPURS_ATTRIBUTE_GROUP_NORMAL && type != PS3_SPURS_ATTRIBUTE_GROUP_EXCLUSIVE)
        return PS3_SPURS_ATTRIBUTE_INVAL;
    return PS3_SPURS_ATTRIBUTE_OK;
}

ps3_spurs_attribute_status ps3_spurs_attribute_initialize(uint32_t address,
    const ps3_spurs_attribute_initialization *arguments,
    unsigned char image[PS3_SPURS_ATTRIBUTE_SIZE])
{
    const ps3_spurs_attribute_status status = ps3_spurs_attribute_validate(address);
    if (status != PS3_SPURS_ATTRIBUTE_OK) return status;
    if (!arguments || !image) return PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    memset(image, 0, PS3_SPURS_ATTRIBUTE_SIZE);
    encode_be32(image, arguments->revision);
    encode_be32(image + 4, arguments->sdk_version);
    encode_be32(image + 8, arguments->n_spus);
    encode_be32(image + 12, (uint32_t)arguments->spu_priority);
    encode_be32(image + 16, (uint32_t)arguments->ppu_priority);
    image[20] = arguments->exit_if_no_work != 0;
    return PS3_SPURS_ATTRIBUTE_OK;
}

ps3_spurs_attribute_status ps3_spurs_attribute_group_flags(
    int32_t type, uint32_t flags, uint32_t *updated)
{
    if (!updated) return PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    if (type == PS3_SPURS_ATTRIBUTE_GROUP_EXCLUSIVE) {
        if (flags & PS3_SPURS_ATTRIBUTE_CONTAINER_FLAG) return PS3_SPURS_ATTRIBUTE_STAT;
        *updated = flags | PS3_SPURS_ATTRIBUTE_EXCLUSIVE_FLAG;
    } else if (type == PS3_SPURS_ATTRIBUTE_GROUP_NORMAL) {
        *updated = flags & ~PS3_SPURS_ATTRIBUTE_EXCLUSIVE_FLAG;
    } else return PS3_SPURS_ATTRIBUTE_INVAL;
    return PS3_SPURS_ATTRIBUTE_OK;
}

ps3_spurs_attribute_status ps3_spurs_attribute_container_flags(
    uint32_t flags, uint32_t *updated)
{
    if (!updated) return PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    if (flags & PS3_SPURS_ATTRIBUTE_EXCLUSIVE_FLAG) return PS3_SPURS_ATTRIBUTE_STAT;
    *updated = flags | PS3_SPURS_ATTRIBUTE_CONTAINER_FLAG;
    return PS3_SPURS_ATTRIBUTE_OK;
}

uint32_t ps3_spurs_attribute_printf_flags(uint32_t flags)
{
    return flags | PS3_SPURS_ATTRIBUTE_PRINTF_FLAG;
}

static void encode_be64(unsigned char *output, uint64_t value)
{
    encode_be32(output, (uint32_t)(value >> 32));
    encode_be32(output + 4, (uint32_t)value);
}

ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_validate_pointers(
    uint32_t attribute, uint32_t policy_module)
{
    if (!attribute) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL;
    if (attribute % PS3_SPURS_WORKLOAD_ATTRIBUTE_ALIGNMENT) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN;
    if (!policy_module) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL;
    if (policy_module % PS3_SPURS_WORKLOAD_POLICY_MODULE_ALIGNMENT) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN;
    return PS3_SPURS_WORKLOAD_ATTRIBUTE_OK;
}

ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_validate_contention(
    uint32_t min_contention, const unsigned char priority[PS3_SPURS_WORKLOAD_PRIORITY_COUNT])
{
    if (!priority) return PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    if (!min_contention) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL;
    for (unsigned i = 0; i < PS3_SPURS_WORKLOAD_PRIORITY_COUNT; ++i)
        if (priority[i] & 0xF0u) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL;
    return PS3_SPURS_WORKLOAD_ATTRIBUTE_OK;
}

ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_build(
    const ps3_spurs_workload_attribute_request *request,
    unsigned char image[PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE])
{
    if (!request || !image) return PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    /* Snapshot first: a valid host caller may alias the request with the image that is zeroed below. */
    const ps3_spurs_workload_attribute_request in = *request;
    ps3_spurs_workload_attribute_status status =
        ps3_spurs_workload_attribute_validate_pointers(in.attribute, in.policy_module);
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK) return status;
    status = ps3_spurs_workload_attribute_validate_contention(in.min_contention, in.priority);
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK) return status;
    memset(image, 0, PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE);
    encode_be32(image + 0x00, in.revision);
    encode_be32(image + 0x04, in.sdk_version);
    encode_be32(image + 0x08, in.policy_module);
    encode_be32(image + 0x0c, in.size);
    encode_be64(image + 0x10, in.data);
    memcpy(image + 0x18, in.priority, PS3_SPURS_WORKLOAD_PRIORITY_COUNT);
    encode_be32(image + 0x20, in.min_contention);
    encode_be32(image + 0x24, in.max_contention);
    return PS3_SPURS_WORKLOAD_ATTRIBUTE_OK;
}

ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_name_patch(
    uint32_t attribute, uint32_t name_class, uint32_t name_instance,
    unsigned char patch[PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE])
{
    if (!patch) return PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT;
    if (!attribute) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL;
    if (attribute % PS3_SPURS_WORKLOAD_ATTRIBUTE_ALIGNMENT) return PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN;
    encode_be32(patch, name_class);
    encode_be32(patch + 4, name_instance);
    return PS3_SPURS_WORKLOAD_ATTRIBUTE_OK;
}
