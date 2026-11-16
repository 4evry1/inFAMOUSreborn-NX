/*
 * ps3recomp - Vulkan backend: the handle slot table
 *
 * Handles the backend returns are small integers and 0 means none
 * (rsx_draw_engine.h:109-208). This is the same two-list arrangement the Metal
 * backend uses (rsx_metal_backend.m:2176-2204): a live slot array indexed by
 * the handle itself, plus a retired list.
 *
 * Why a retired list and not "free at release": a command buffer that is still
 * recording may name the object, so the vtable's *_release members only move a
 * slot to the retired list, and the objects are destroyed at the next
 * fence-passed boundary, via rsx_vk_handle_collect. In S1 that boundary is the
 * end of every submit_and_wait, and shutdown destroys whatever is left.
 */
#include <stdlib.h>
#include <string.h>

#include "rsx_vk.h"

/* The slot struct is private to this file and rsx_vk_target.c's destroy path is
 * the only thing that knows how to free what a slot names, so collect and
 * destroy_all ask it for each id. */
void rsx_vk_slot_destroy(rsx_vk_device* d, rsx_vk_slot* slot);

struct rsx_vk_handle {
    rsx_vk_slot* slots;
    uint8_t* live;
    uint32_t capacity;
    uint32_t largest;      /* highest id ever allocated, inclusive; 0 = none */
    uint32_t live_count;
    uint32_t retired_count;
    uint32_t* retired;
    uint32_t retired_capacity;
};

rsx_vk_handle* rsx_vk_handles_create(uint32_t capacity)
{
    rsx_vk_handle* h;
    if (capacity < 8)
        capacity = 8;
    h = (rsx_vk_handle*)calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->slots = (rsx_vk_slot*)calloc(capacity, sizeof(*h->slots));
    h->live = (uint8_t*)calloc(capacity, 1);
    h->retired = (uint32_t*)calloc(capacity, sizeof(*h->retired));
    if (!h->slots || !h->live || !h->retired) {
        free(h->slots);
        free(h->live);
        free(h->retired);
        free(h);
        return NULL;
    }
    h->capacity = capacity;
    h->retired_capacity = capacity;
    return h;
}

void rsx_vk_handles_destroy(rsx_vk_handle* h)
{
    if (!h)
        return;
    free(h->slots);
    free(h->live);
    free(h->retired);
    free(h);
}

u32 rsx_vk_handle_alloc(rsx_vk_handle* h, rsx_vk_slot** out)
{
    uint32_t i;
    if (!h || !out)
        return 0;
    /* Handle ids are 1-based and never 0. Reuse a hole before growing so a
     * long run of create/release cycles does not walk the id space. */
    for (i = 1; i < h->capacity; i++) {
        if (!h->live[i]) {
            h->live[i] = 1;
            h->live_count++;
            if (i > h->largest)
                h->largest = i;
            memset(&h->slots[i], 0, sizeof(h->slots[i]));
            h->slots[i].u.color.layout = -1;
            *out = &h->slots[i];
            return i;
        }
    }
    return 0; /* table full: the caller logs and returns 0, never a wrap */
}

rsx_vk_slot* rsx_vk_handle_get(rsx_vk_handle* h, u32 id)
{
    if (!h || id == 0 || id >= h->capacity || h->live[id] != 1)
        return NULL;
    return &h->slots[id];
}

void rsx_vk_handle_retire(rsx_vk_handle* h, u32 id)
{
    if (!h || id == 0 || id >= h->capacity || h->live[id] != 1)
        return;
    if (h->retired_count >= h->retired_capacity) {
        /* Grow rather than drop: dropping would leak the object, which is
         * exactly the thing this table exists to prevent. */
        uint32_t cap = h->retired_capacity * 2;
        uint32_t* grown = (uint32_t*)realloc(h->retired, cap * sizeof(uint32_t));
        if (!grown)
            return;
        h->retired = grown;
        h->retired_capacity = cap;
    }
    h->retired[h->retired_count++] = id;
    /* Root (2026-11-12, Decision 95): 2 = retired, waiting for rsx_vk_handle_collect. The id used to become free
     * here (0), so the next create could take it, wipe the slot (leaking the old objects) and then have ITS
     * objects destroyed by the collect meant for the old ones. Seen on the console (run rv26) as a red intro
     * movie: a movie plane's fresh image was destroyed and the unit sampled the white null texture. A retired id
     * is neither gettable nor allocatable until it has been collected. */
    h->live[id] = 2;
    h->live_count--;
}

u32 rsx_vk_handle_live(const rsx_vk_handle* h)
{
    return h ? h->live_count : 0;
}

u32 rsx_vk_handle_retired(const rsx_vk_handle* h)
{
    return h ? h->retired_count : 0;
}

u32 rsx_vk_handle_capacity(const rsx_vk_handle* h)
{
    return h ? h->capacity : 0;
}

void rsx_vk_handle_collect(rsx_vk_device* d)
{
    uint32_t i;
    if (!d || !d->handles)
        return;
    for (i = 0; i < d->handles->retired_count; i++) {
        uint32_t id = d->handles->retired[i];
        if (id < d->handles->capacity && d->handles->live[id] == 2) {
            rsx_vk_slot_destroy(d, &d->handles->slots[id]);
            d->handles->live[id] = 0;   /* only now may the id be given out again */
        }
    }
    d->handles->retired_count = 0;
}

void rsx_vk_handle_destroy_all(rsx_vk_device* d)
{
    uint32_t i;
    if (!d || !d->handles)
        return;
    rsx_vk_handle_collect(d);
    for (i = 1; i < d->handles->capacity; i++) {
        if (d->handles->live[i] != 1)
            continue;
        rsx_vk_slot_destroy(d, &d->handles->slots[i]);
        d->handles->live[i] = 0;
    }
    d->handles->live_count = 0;
    d->handles->retired_count = 0;
}
