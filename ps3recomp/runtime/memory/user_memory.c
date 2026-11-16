#include "user_memory.h"
#include <string.h>
static ps3_user_memory_result operation_result(ps3_user_memory_status status,
                                               ps3_guest_memory_status memory_status) {
    return (ps3_user_memory_result) {status, memory_status};
}
static int is_initialized(const ps3_user_memory *m) {
    return m && m->slots && m->capacity && m->config.guard.lock && m->config.guard.unlock;
}
static uint64_t align_address(uint64_t value,uint64_t alignment) {
    return (value+alignment-1)&~(alignment-1);
}
ps3_user_memory_status ps3_user_memory_init(
    ps3_user_memory *m,
    const ps3_user_memory_config *c,
    ps3_user_memory_slot *slots,
    size_t capacity) {
    if (!m || !c || !slots || !capacity || capacity>SIZE_MAX/sizeof(*slots) ||
    !c->memory.commit || !c->memory.decommit || !c->guard.lock || !c->guard.unlock ||
    c->window_begin>=c->window_end || c->window_end>PS3_GUEST_ADDRESS_BYTES ||
    c->window_begin%PS3_GUEST_PAGE_BYTES || c->window_end%PS3_GUEST_PAGE_BYTES ||
    !c->quota_bytes || c->quota_bytes>c->window_end-c->window_begin) return PS3_USER_MEMORY_INVALID_ARGUMENT;
    ps3_user_memory value={
        .config=*c,.slots=slots,.capacity=capacity,.last_memory_status=PS3_GUEST_MEMORY_OK
    };
    memset(slots,0,capacity*sizeof(*slots));
    *m=value;
    return PS3_USER_MEMORY_OK;
}
ps3_user_memory_result ps3_user_memory_allocate(
    ps3_user_memory *m,
    uint64_t size,
    uint64_t alignment,
    uint32_t *address) {
    if (!is_initialized(m) || !address || (alignment!=PS3_USER_MEMORY_ALIGN_64K && alignment!=PS3_USER_MEMORY_ALIGN_1M) || !size || size%alignment) return operation_result(PS3_USER_MEMORY_INVALID_ARGUMENT, PS3_GUEST_MEMORY_OK);
    m->config.guard.lock(m->config.guard.context);
    ps3_user_memory_status result=PS3_USER_MEMORY_OK;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_OK;
    size_t empty=m->capacity;
    if (size>m->config.quota_bytes-m->used_bytes) {
        result=PS3_USER_MEMORY_QUOTA_EXCEEDED;
        goto done;
    }
    for (size_t i=0;i<m->capacity;i++) if (!m->slots[i].size) {
        empty=i;
        break;
    }
    if (empty==m->capacity) {
        result=PS3_USER_MEMORY_CAPACITY_EXCEEDED;
        goto done;
    }
    uint64_t candidate=align_address(m->config.window_begin,alignment);
    for (;;) {
        if (candidate>=m->config.window_end || size>m->config.window_end-candidate) {
            result=PS3_USER_MEMORY_NO_SPACE;
            goto done;
        }
        uint64_t next=candidate;
        for (size_t i=0;i<m->capacity;i++) {
            ps3_user_memory_slot s=m->slots[i];
            if (s.size && candidate<(uint64_t)s.address+s.size && (uint64_t)s.address<candidate+size) {
                uint64_t end=align_address((uint64_t)s.address+s.size,alignment);
                if (end>next)next=end;
            }
        }
        if (next==candidate) break;
        candidate=next;
    }
    memory_status=m->config.memory.commit(m->config.memory.context,(uint32_t)candidate,size);
    m->last_memory_status=memory_status;
    if (memory_status!=PS3_GUEST_MEMORY_OK) {
        result=PS3_USER_MEMORY_MEMORY_FAILURE;
        goto done;
    }
    m->slots[empty]=(ps3_user_memory_slot) {
        .address=(uint32_t)candidate,.size=size
    };
    m->used_bytes+=size;
    m->allocation_count++;
    *address=(uint32_t)candidate;
    done:
    m->config.guard.unlock(m->config.guard.context);
    return operation_result(result, memory_status);
}
static ps3_user_memory_status free_slot(ps3_user_memory *m,size_t i) {
    ps3_user_memory_slot s=m->slots[i];
    m->last_memory_status=m->config.memory.decommit(m->config.memory.context,s.address,s.size);
    if (m->last_memory_status!=PS3_GUEST_MEMORY_OK) return PS3_USER_MEMORY_MEMORY_FAILURE;
    m->used_bytes-=s.size;
    m->allocation_count--;
    m->slots[i]=(ps3_user_memory_slot) {
        0
    };
    return PS3_USER_MEMORY_OK;
}
ps3_user_memory_result ps3_user_memory_free(
    ps3_user_memory *m,
    uint32_t address) {
    if (!is_initialized(m)) return operation_result(PS3_USER_MEMORY_INVALID_ARGUMENT, PS3_GUEST_MEMORY_OK);
    m->config.guard.lock(m->config.guard.context);
    ps3_user_memory_status result=PS3_USER_MEMORY_NOT_OWNED;
    for (size_t i=0;i<m->capacity;i++)if (m->slots[i].size && m->slots[i].address==address) {
        result=free_slot(m,i);
        break;
    }
    ps3_guest_memory_status memory_status=result==PS3_USER_MEMORY_MEMORY_FAILURE ? m->last_memory_status : PS3_GUEST_MEMORY_OK;
    m->config.guard.unlock(m->config.guard.context);
    return operation_result(result, memory_status);
}
ps3_user_memory_status ps3_user_memory_query(
    ps3_user_memory *m,
    ps3_user_memory_info *out) {
    if (!is_initialized(m)||!out) return PS3_USER_MEMORY_INVALID_ARGUMENT;
    m->config.guard.lock(m->config.guard.context);
    *out=(ps3_user_memory_info) {
        .quota_bytes=m->config.quota_bytes,.used_bytes=m->used_bytes,.available_bytes=m->config.quota_bytes-m->used_bytes,.allocation_count=m->allocation_count,.last_memory_status=m->last_memory_status
    };
    m->config.guard.unlock(m->config.guard.context);
    return PS3_USER_MEMORY_OK;
}
ps3_user_memory_status ps3_user_memory_attach_charges(ps3_user_memory *m, ps3_user_memory_charge_record *records,
                                                      size_t count) {
    if (!is_initialized(m) || !records || !count || count > SIZE_MAX / sizeof *records) return PS3_USER_MEMORY_INVALID_ARGUMENT;
    m->config.guard.lock(m->config.guard.context);
    ps3_user_memory_status result = PS3_USER_MEMORY_INVALID_ARGUMENT;
    if (!m->charges) {
        memset(records, 0, count * sizeof *records);
        m->charges = records;
        m->charge_capacity = count;
        result = PS3_USER_MEMORY_OK;
    }
    m->config.guard.unlock(m->config.guard.context);
    return result;
}
ps3_user_memory_status ps3_user_memory_charge(ps3_user_memory *m, uint64_t size, ps3_user_memory_charge_token *token) {
    if (!is_initialized(m) || !size || !token) return PS3_USER_MEMORY_INVALID_ARGUMENT;
    m->config.guard.lock(m->config.guard.context);
    ps3_user_memory_status result = PS3_USER_MEMORY_CAPACITY_EXCEEDED;
    if (size > m->config.quota_bytes - m->used_bytes) result = PS3_USER_MEMORY_QUOTA_EXCEEDED;
    else if (m->next_charge_id != UINT64_MAX)
        for (size_t i = 0; i < m->charge_capacity; ++i) if (!m->charges[i].id) {
            m->charges[i] = (ps3_user_memory_charge_record){++m->next_charge_id, size};
            m->used_bytes += size;
            *token = (ps3_user_memory_charge_token){m, i, m->charges[i].id};
            result = PS3_USER_MEMORY_OK;
            break;
        }
    m->config.guard.unlock(m->config.guard.context);
    return result;
}
ps3_user_memory_status ps3_user_memory_uncharge(ps3_user_memory *m, ps3_user_memory_charge_token *token) {
    if (!is_initialized(m) || !token) return PS3_USER_MEMORY_INVALID_ARGUMENT;
    m->config.guard.lock(m->config.guard.context);
    ps3_user_memory_status result = PS3_USER_MEMORY_NOT_OWNED;
    if (token->owner == m && token->id && token->slot < m->charge_capacity && m->charges[token->slot].id == token->id) {
        m->used_bytes -= m->charges[token->slot].size;
        m->charges[token->slot] = (ps3_user_memory_charge_record){0, 0};
        *token = (ps3_user_memory_charge_token){NULL, 0, 0};
        result = PS3_USER_MEMORY_OK;
    }
    m->config.guard.unlock(m->config.guard.context);
    return result;
}
ps3_user_memory_result ps3_user_memory_cleanup(
    ps3_user_memory *m) {
    if (!is_initialized(m)) return operation_result(PS3_USER_MEMORY_INVALID_ARGUMENT, PS3_GUEST_MEMORY_OK);
    m->config.guard.lock(m->config.guard.context);
    for (size_t i = 0; i < m->charge_capacity; ++i) if (m->charges[i].id) {
        m->config.guard.unlock(m->config.guard.context);
        return operation_result(PS3_USER_MEMORY_CHARGES_OUTSTANDING, PS3_GUEST_MEMORY_OK);
    }
    ps3_user_memory_status result=PS3_USER_MEMORY_OK;
    ps3_guest_memory_status first_failure=PS3_GUEST_MEMORY_OK;
    for (size_t i=0;i<m->capacity;i++)if (m->slots[i].size && free_slot(m,i)!=PS3_USER_MEMORY_OK) {
        result=PS3_USER_MEMORY_MEMORY_FAILURE;
        if (first_failure==PS3_GUEST_MEMORY_OK)first_failure=m->last_memory_status;
    }
    if (result!=PS3_USER_MEMORY_OK)m->last_memory_status=first_failure;
    m->config.guard.unlock(m->config.guard.context);
    return operation_result(result, first_failure);
}
