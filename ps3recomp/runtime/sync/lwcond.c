#include "lwcond.h"
#include <string.h>
enum { COND_LIVE=1, COND_UNPUBLISHED=2, COND_RETIRING=3 };
enum { CALL_ACTIVE=1, CALL_RETAINED=2 };
static ps3_lwcond_result result(ps3_lwcond_status status)
{
    return (ps3_lwcond_result){.status=status,.memory_status=PS3_GUEST_MEMORY_OK,
        .wait_status=PS3_WAIT_READY,.mutex_status=PS3_LWMUTEX_OK};
}
static ps3_lwcond_result memory_failure(ps3_guest_memory_status status)
{
    ps3_lwcond_result r=result(PS3_LWCOND_MEMORY_FAILURE); r.memory_status=status; r.must_not_resume=1; return r;
}
static ps3_lwcond_result mutex_result(ps3_lwmutex_result source)
{
    ps3_lwcond_status status=PS3_LWCOND_MUTEX_FAILURE;
    switch(source.status) {
    case PS3_LWMUTEX_OK: status=PS3_LWCOND_OK; break;
    case PS3_LWMUTEX_NOT_FOUND: status=PS3_LWCOND_NOT_FOUND; break;
    case PS3_LWMUTEX_NOT_OWNER: status=PS3_LWCOND_NOT_OWNER; break;
    case PS3_LWMUTEX_RESOURCE_EXHAUSTED: status=PS3_LWCOND_RESOURCE_EXHAUSTED; break;
    case PS3_LWMUTEX_BUSY: status=PS3_LWCOND_BUSY; break;
    case PS3_LWMUTEX_INCONSISTENT: status=PS3_LWCOND_INCONSISTENT; break;
    case PS3_LWMUTEX_CLEANUP_REQUIRED: status=PS3_LWCOND_CLEANUP_REQUIRED; break;
    default: break;
    }
    ps3_lwcond_result r=result(status); r.memory_status=source.memory_status;
    r.wait_status=source.wait_status; r.mutex_status=source.status;
    r.must_not_resume=status==PS3_LWCOND_MUTEX_FAILURE || status==PS3_LWCOND_INCONSISTENT || status==PS3_LWCOND_CLEANUP_REQUIRED;
    return r;
}
static int valid(ps3_lwcond_registry *r) { return r && r->initialized; }
static int credit(ps3_lwcond_report *report,size_t count)
{
    return report && report->publications.count<=PS3_SYNC_PUBLICATION_CAPACITY &&
        count<=PS3_SYNC_PUBLICATION_CAPACITY-report->publications.count;
}
static int cancelled(ps3_wait_cancel token) { return token.is_cancelled && token.is_cancelled(token.context); }
typedef struct park_cancellation {
    ps3_wait_cancel instance_stop, ordinary_cancel;
} park_cancellation;
static int park_cancelled(void *context)
{
    park_cancellation *tokens=context;
    return cancelled(tokens->instance_stop) || cancelled(tokens->ordinary_cancel);
}
static void encode32(unsigned char *p,uint32_t n)
{
    for(unsigned i=0;i<4;++i) { p[3-i]=(unsigned char)n; n>>=8; }
}
static size_t find(ps3_lwcond_registry *r,uint32_t address)
{
    for(size_t i=0;i<r->object_capacity;++i) if(r->objects[i].state && r->objects[i].address==address) return i;
    return r->object_capacity;
}
static ps3_lwcond_result check_image(ps3_lwcond_registry *r,size_t object)
{
    ps3_lwcond_object *o=&r->objects[object];
    if(o->state!=COND_LIVE) { ps3_lwcond_result v=result(PS3_LWCOND_CLEANUP_REQUIRED); v.cleanup_retained=v.must_not_resume=1; return v; }
    unsigned char actual[8],expected[8]; encode32(expected,o->mutex_address); encode32(expected+4,o->queue_id);
    ps3_guest_memory_status status=ps3_atomic_read(r->mutexes->domain,o->address,actual,8);
    if(status!=PS3_GUEST_MEMORY_OK) return memory_failure(status);
    if(memcmp(actual,expected,8)) { ps3_lwcond_result v=result(PS3_LWCOND_INCONSISTENT); v.must_not_resume=1; return v; }
    return result(PS3_LWCOND_OK);
}
static ps3_lwcond_result publish(ps3_lwcond_registry *r,size_t object,uint32_t queue,ps3_lwcond_report *report)
{
    unsigned char image[8]; encode32(image,r->objects[object].mutex_address); encode32(image+4,queue);
    ps3_guest_memory_status status=ps3_atomic_write(r->mutexes->domain,r->objects[object].address,image,8);
    if(status!=PS3_GUEST_MEMORY_OK) return memory_failure(status);
    ps3_sync_publication *entry=&report->publications.writes[report->publications.count++];
    entry->address=r->objects[object].address; entry->size=8; memcpy(entry->bytes,image,8);
    return result(PS3_LWCOND_OK);
}
static ps3_lwcond_result release_object(ps3_lwcond_registry *r,size_t object)
{
    ps3_lwcond_object *o=&r->objects[object];
    if(o->binding.retained) {
        ps3_lwcond_result v=mutex_result(ps3_lwmutex_condition_unbind(r->mutexes,&o->binding));
        if(v.status!=PS3_LWCOND_OK) { v.status=PS3_LWCOND_CLEANUP_REQUIRED; v.cleanup_retained=v.must_not_resume=1; return v; }
    }
    if(o->has_pin) {
        ps3_guest_memory_status status=ps3_atomic_pin_release(r->mutexes->domain,&o->pin);
        if(status!=PS3_GUEST_MEMORY_OK) { ps3_lwcond_result v=memory_failure(status); v.status=PS3_LWCOND_CLEANUP_REQUIRED; v.cleanup_retained=1; return v; }
    }
    uint64_t generation=o->generation; *o=(ps3_lwcond_object){.generation=generation}; return result(PS3_LWCOND_OK);
}
ps3_lwcond_result ps3_lwcond_init(ps3_lwcond_registry *r,ps3_lwmutex_registry *mutexes,
    ps3_instance_guard guard,ps3_wait_port wait,ps3_lwcond_thread_existence threads,
    ps3_lwcond_object *objects,size_t object_capacity,ps3_lwcond_waiter *waiters,size_t waiter_capacity)
{
    if(!r || !mutexes || !mutexes->initialized || !guard.lock || !guard.unlock ||
        guard.context==mutexes->guard.context || guard.context==mutexes->domain->guard.context ||
        !wait.now || !wait.wait || !wait.notify_all || wait.context==mutexes->wait.context || !threads.query ||
        !objects || !object_capacity || object_capacity>SIZE_MAX/sizeof(*objects) ||
        !waiters || !waiter_capacity || waiter_capacity>PS3_SYNC_PUBLICATION_CAPACITY ||
        waiter_capacity>SIZE_MAX/sizeof(*waiters)) return result(PS3_LWCOND_INVALID_ARGUMENT);
    memset(objects,0,object_capacity*sizeof(*objects)); memset(waiters,0,waiter_capacity*sizeof(*waiters));
    *r=(ps3_lwcond_registry){.mutexes=mutexes,.guard=guard,.wait=wait,.threads=threads,
        .objects=objects,.object_capacity=object_capacity,.waiters=waiters,.waiter_capacity=waiter_capacity,
        .next_sequence=1,.next_queue_id=1,.initialized=1};
    return result(PS3_LWCOND_OK);
}
ps3_lwcond_result ps3_lwcond_create(ps3_lwcond_registry *r,uint32_t address,uint32_t mutex,
    const unsigned char name[8],ps3_lwcond_report *report)
{
    if(!valid(r) || !name || !report || address%4 || (uint64_t)address+8>PS3_GUEST_ADDRESS_BYTES ||
        ((uint64_t)address<(uint64_t)mutex+24 && (uint64_t)mutex<(uint64_t)address+8)) return result(PS3_LWCOND_INVALID_ARGUMENT);
    if(!credit(report,1)) return result(PS3_LWCOND_RESOURCE_EXHAUSTED);
    r->guard.lock(r->guard.context); ps3_lwcond_result v=result(PS3_LWCOND_RESOURCE_EXHAUSTED); size_t slot=r->object_capacity;
    for(size_t i=0;i<r->object_capacity;++i) {
        ps3_lwcond_object *o=&r->objects[i];
        if(o->state && (uint64_t)address<(uint64_t)o->address+8 && (uint64_t)o->address<(uint64_t)address+8) {
            v=result(o->state==COND_LIVE?PS3_LWCOND_UNSUPPORTED:PS3_LWCOND_CLEANUP_REQUIRED);
            v.must_not_resume=1; v.cleanup_retained=o->state!=COND_LIVE; goto create_done;
        }
        if(!o->state && o->generation!=UINT64_MAX && slot==r->object_capacity) slot=i;
    }
    if(slot==r->object_capacity || !r->next_queue_id || r->next_queue_id>=PS3_LWMUTEX_RESERVED) goto create_done;
    ps3_lwcond_object *o=&r->objects[slot];
    *o=(ps3_lwcond_object){.generation=o->generation+1,.address=address,.mutex_address=mutex,
        .queue_id=r->next_queue_id++,.state=COND_UNPUBLISHED}; memcpy(o->name,name,8);
    ps3_guest_memory_status status=ps3_atomic_pin_acquire(r->mutexes->domain,address,8,&o->pin);
    if(status!=PS3_GUEST_MEMORY_OK) { v=memory_failure(status); goto create_rollback; }
    o->has_pin=1;
    v=mutex_result(ps3_lwmutex_condition_bind(r->mutexes,mutex,&o->binding));
    if(v.status!=PS3_LWCOND_OK) goto create_rollback;
    v=publish(r,slot,o->queue_id,report);
    if(v.status!=PS3_LWCOND_OK) goto create_rollback;
    o->state=COND_LIVE; goto create_done;
create_rollback: {
    ps3_lwcond_result cleanup=release_object(r,slot);
    if(cleanup.status!=PS3_LWCOND_OK) v=cleanup;
}
create_done:
    r->guard.unlock(r->guard.context); return v;
}
static ps3_lwcond_result retire(ps3_lwcond_registry *r,size_t index,ps3_lwcond_report *report)
{
    ps3_lwcond_object *o=&r->objects[index];
    if(o->active_waiters) return result(PS3_LWCOND_BUSY);
    if(o->state==COND_LIVE) {
        ps3_lwcond_result v=check_image(r,index); if(v.status!=PS3_LWCOND_OK) return v;
        if(!credit(report,1)) return result(PS3_LWCOND_RESOURCE_EXHAUSTED);
        v=publish(r,index,PS3_LWMUTEX_DEAD,report); if(v.status!=PS3_LWCOND_OK) return v;
        o->state=COND_RETIRING;
    }
    return release_object(r,index);
}
ps3_lwcond_result ps3_lwcond_destroy(ps3_lwcond_registry *r,uint32_t address,ps3_lwcond_report *report)
{
    if(!valid(r) || !report) return result(PS3_LWCOND_INVALID_ARGUMENT);
    r->guard.lock(r->guard.context); size_t object=find(r,address);
    ps3_lwcond_result v=object==r->object_capacity?result(PS3_LWCOND_NOT_FOUND):retire(r,object,report);
    r->guard.unlock(r->guard.context); return v;
}
static int waiter_belongs(ps3_lwcond_registry *r,size_t waiter,size_t object)
{
    return r->waiters[waiter].state && r->waiters[waiter].object_index==object &&
        r->waiters[waiter].object_generation==r->objects[object].generation;
}
static void retire_waiter(ps3_lwcond_registry *r,size_t slot)
{
    --r->objects[r->waiters[slot].object_index].active_waiters; r->waiters[slot]=(ps3_lwcond_waiter){0};
}
static ps3_lwcond_result wait_provider_failure(ps3_wait_status status)
{
    ps3_lwcond_result v=result(PS3_LWCOND_WAIT_FAILURE); v.wait_status=status; v.must_not_resume=1; return v;
}
ps3_lwcond_result ps3_lwcond_wait(ps3_lwcond_registry *r,uint32_t address,ps3_lwmutex_thread thread,
    uint64_t timeout,ps3_wait_cancel cancel,ps3_wait_cancel stop,ps3_lwcond_report *report)
{
    if(!valid(r) || !report || !thread.id || thread.id>=PS3_LWMUTEX_RESERVED) return result(PS3_LWCOND_INVALID_ARGUMENT);
    if(!credit(report,3)) return result(PS3_LWCOND_RESOURCE_EXHAUSTED);
    r->guard.lock(r->guard.context); size_t object=find(r,address),slot=r->waiter_capacity;
    ps3_lwcond_result v=result(PS3_LWCOND_NOT_FOUND);
    if(object==r->object_capacity) goto wait_done;
    v=check_image(r,object); if(v.status!=PS3_LWCOND_OK) goto wait_done;
    for(size_t i=0;i<r->waiter_capacity;++i) {
        if(r->waiters[i].state && r->waiters[i].thread.id==thread.id) { v=result(PS3_LWCOND_BUSY); goto wait_done; }
        if(!r->waiters[i].state && slot==r->waiter_capacity) slot=i;
    }
    if(slot==r->waiter_capacity || r->next_sequence==UINT64_MAX) { v=result(PS3_LWCOND_RESOURCE_EXHAUSTED); goto wait_done; }
    if(cancelled(stop)) { v=result(PS3_LWCOND_STOPPED); v.must_not_resume=1; goto wait_done; }
    if(cancelled(cancel)) { v=result(PS3_LWCOND_CANCELLED); goto wait_done; }
    uint64_t deadline=UINT64_MAX,now;
    if(timeout) {
        ps3_wait_status status=r->wait.now(r->wait.context,&now);
        if(status!=PS3_WAIT_READY) { v=wait_provider_failure(status); goto wait_done; }
        if(now==UINT64_MAX || timeout>(UINT64_MAX-1-now)/1000) { v=result(PS3_LWCOND_INVALID_ARGUMENT); goto wait_done; }
        deadline=now+timeout*1000;
    }
    ps3_lwcond_waiter *w=&r->waiters[slot];
    *w=(ps3_lwcond_waiter){.object_index=object,.object_generation=r->objects[object].generation,
        .sequence=r->next_sequence++,.deadline=deadline,.thread=thread,.outcome=PS3_LWCOND_QUEUED,.state=CALL_ACTIVE};
    v=mutex_result(ps3_lwmutex_condition_release(r->mutexes,&r->objects[object].binding,thread,&w->reservation,&report->publications));
    if(v.status!=PS3_LWCOND_OK) { *w=(ps3_lwcond_waiter){0}; goto wait_done; }
    ++r->objects[object].active_waiters;
    int stopped=0;
    /* The adapter guarantees progress for the token it receives, not for a
     * separate instance-stop callback. This context stays on its owner's stack
     * only through synchronous wait calls; no registry retains it. */
    park_cancellation park_tokens={stop,cancel};
    ps3_wait_cancel park_token=(stop.is_cancelled || cancel.is_cancelled)?
        (ps3_wait_cancel){&park_tokens,park_cancelled}:(ps3_wait_cancel){0};
    for(;;) {
        stopped=cancelled(stop);
        if(stopped) {
            if(w->outcome==PS3_LWCOND_QUEUED) w->outcome=PS3_LWCOND_FAILED;
            break;
        }
        if(w->outcome!=PS3_LWCOND_QUEUED) break;
        if(cancelled(cancel)) { w->outcome=PS3_LWCOND_WAIT_CANCELLED; break; }
        ps3_wait_status status=r->wait.now(r->wait.context,&now);
        if(status!=PS3_WAIT_READY) { w->outcome=PS3_LWCOND_FAILED; v=wait_provider_failure(status); break; }
        if(deadline!=UINT64_MAX && now>=deadline) { w->outcome=PS3_LWCOND_EXPIRED; break; }
        status=r->wait.wait(r->wait.context,&r->guard,deadline,park_token);
        if(status==PS3_WAIT_READY || status==PS3_WAIT_TIMEOUT) continue;
        if(cancelled(stop)) {
            stopped=1;
            if(w->outcome==PS3_LWCOND_QUEUED) w->outcome=PS3_LWCOND_FAILED;
            break;
        }
        if(w->outcome!=PS3_LWCOND_QUEUED) {
            if(status==PS3_WAIT_CANCELLED) continue;
            v=wait_provider_failure(status); break;
        }
        if(status==PS3_WAIT_CANCELLED) { w->outcome=PS3_LWCOND_WAIT_CANCELLED; break; }
        w->outcome=PS3_LWCOND_FAILED; v=wait_provider_failure(status); break;
    }
    int abort_required=stopped;
    if(!stopped && w->outcome!=PS3_LWCOND_SELECTED) {
        ps3_lwcond_result promotion=mutex_result(ps3_lwmutex_condition_promote(r->mutexes,&w->reservation,&report->publications));
        if(promotion.status!=PS3_LWCOND_OK) {
            if(v.wait_status!=PS3_WAIT_READY) promotion.wait_status=v.wait_status;
            v=promotion; v.must_not_resume=1; abort_required=1;
        }
    }
    ps3_lwcond_outcome outcome=w->outcome;
    /* The stable slot remains active/pinned while its owner restores outside
     * this guard, so a signaler holding the guest mutex cannot deadlock it. */
    r->guard.unlock(r->guard.context);
    ps3_lwmutex_result restored=abort_required?
        ps3_lwmutex_condition_abort(r->mutexes,&w->reservation,&report->publications):
        ps3_lwmutex_condition_reacquire(r->mutexes,&w->reservation,stop,&report->publications);
    r->guard.lock(r->guard.context);
    if(restored.status!=PS3_LWMUTEX_OK) {
        ps3_lwcond_result failure=mutex_result(restored);
        if(v.wait_status!=PS3_WAIT_READY && failure.wait_status==PS3_WAIT_READY) failure.wait_status=v.wait_status;
        if(v.memory_status!=PS3_GUEST_MEMORY_OK && failure.memory_status==PS3_GUEST_MEMORY_OK) failure.memory_status=v.memory_status;
        v=failure; v.cleanup_retained=v.must_not_resume=1; w->state=CALL_RETAINED;
        if(stopped || restored.wait_status==PS3_WAIT_CANCELLED) v.status=PS3_LWCOND_STOPPED;
    } else {
        v.mutex_reacquired=1;
        if(outcome==PS3_LWCOND_SELECTED && !v.must_not_resume) v.status=PS3_LWCOND_OK;
        if(outcome==PS3_LWCOND_EXPIRED && !v.must_not_resume) v.status=PS3_LWCOND_TIMED_OUT;
        if(outcome==PS3_LWCOND_WAIT_CANCELLED && !v.must_not_resume) v.status=PS3_LWCOND_CANCELLED;
        retire_waiter(r,slot);
    }
    v.condition_outcome=outcome;
wait_done:
    r->guard.unlock(r->guard.context); return v;
}
static size_t select_waiter(ps3_lwcond_registry *r,size_t object,uint64_t now,uint64_t target,const unsigned char *excluded)
{
    size_t chosen=r->waiter_capacity;
    int priority=(r->objects[object].binding.attributes&15)==PS3_LWMUTEX_PRIORITY;
    for(size_t i=0;i<r->waiter_capacity;++i) {
        ps3_lwcond_waiter *w=&r->waiters[i];
        if(excluded[i] || !waiter_belongs(r,i,object) || w->state!=CALL_ACTIVE || w->outcome!=PS3_LWCOND_QUEUED) continue;
        if(w->deadline!=UINT64_MAX && now>=w->deadline) { w->outcome=PS3_LWCOND_EXPIRED; continue; }
        if(target && w->thread.id!=target) continue;
        if(chosen==r->waiter_capacity || (priority && w->thread.priority<r->waiters[chosen].thread.priority) ||
            ((!priority || w->thread.priority==r->waiters[chosen].thread.priority) && w->sequence<r->waiters[chosen].sequence)) chosen=i;
    }
    return chosen;
}
static ps3_lwcond_result signal_condition(ps3_lwcond_registry *r,uint32_t address,uint64_t target,int all,ps3_lwcond_report *report)
{
    if(!valid(r) || !report) return result(PS3_LWCOND_INVALID_ARGUMENT);
    if(target>=PS3_LWMUTEX_RESERVED) { ps3_lwcond_result v=result(PS3_LWCOND_UNSUPPORTED); v.must_not_resume=1; return v; }
    if(!credit(report,all?r->waiter_capacity:1)) return result(PS3_LWCOND_RESOURCE_EXHAUSTED);
    r->guard.lock(r->guard.context); size_t object=find(r,address);
    ps3_lwcond_result v=result(PS3_LWCOND_NOT_FOUND); report->selected_count=0;
    if(object==r->object_capacity) goto signal_done;
    v=check_image(r,object); if(v.status!=PS3_LWCOND_OK) goto signal_done;
    if(target) {
        unsigned exists=0;
        if(r->threads.query(r->threads.context,(uint32_t)target,&exists)!=PS3_LWCOND_THREAD_OK || exists>1) {
            v=result(PS3_LWCOND_THREAD_FAILURE); v.must_not_resume=1; goto signal_done;
        }
        if(!exists) { v=result(PS3_LWCOND_TARGET_NOT_FOUND); goto signal_done; }
    }
    uint64_t now; ps3_wait_status clock=r->wait.now(r->wait.context,&now);
    if(clock!=PS3_WAIT_READY) { v=wait_provider_failure(clock); goto signal_done; }
    ps3_lwmutex_condition_wait *tokens[PS3_SYNC_PUBLICATION_CAPACITY];
    size_t slots[PS3_SYNC_PUBLICATION_CAPACITY],count=0;
    unsigned char excluded[PS3_SYNC_PUBLICATION_CAPACITY]={0};
    /* Build selection order without publishing any outcome before admission. */
    while(count<(all?r->waiter_capacity:1)) {
        size_t chosen=select_waiter(r,object,now,target,excluded);
        if(chosen==r->waiter_capacity) break;
        slots[count]=chosen; tokens[count++]=&r->waiters[chosen].reservation;
        excluded[chosen]=1;
    }
    if(!count) { v=result(target?PS3_LWCOND_TARGET_NOT_WAITING:PS3_LWCOND_OK); r->wait.notify_all(r->wait.context); goto signal_done; }
    size_t promoted=0;
    ps3_lwmutex_result admission=ps3_lwmutex_condition_promote_many(r->mutexes,tokens,count,&promoted,&report->publications);
    for(size_t i=0;i<promoted;++i) r->waiters[slots[i]].outcome=PS3_LWCOND_SELECTED;
    report->selected_count=promoted;
    v=mutex_result(admission);
    if(v.status!=PS3_LWCOND_OK) v.must_not_resume=1;
    r->wait.notify_all(r->wait.context);
signal_done:
    r->guard.unlock(r->guard.context); return v;
}
ps3_lwcond_result ps3_lwcond_signal(ps3_lwcond_registry *r,uint32_t ea,ps3_lwcond_report *report)
{ return signal_condition(r,ea,0,0,report); }
ps3_lwcond_result ps3_lwcond_signal_all(ps3_lwcond_registry *r,uint32_t ea,ps3_lwcond_report *report)
{ return signal_condition(r,ea,0,1,report); }
ps3_lwcond_result ps3_lwcond_signal_to(ps3_lwcond_registry *r,uint32_t ea,uint64_t target,ps3_lwcond_report *report)
{
    if(!target) {
        if(!valid(r) || !report) return result(PS3_LWCOND_INVALID_ARGUMENT);
        r->guard.lock(r->guard.context); size_t object=find(r,ea);
        ps3_lwcond_result v=object==r->object_capacity?result(PS3_LWCOND_NOT_FOUND):check_image(r,object);
        if(v.status==PS3_LWCOND_OK) v=result(PS3_LWCOND_TARGET_NOT_FOUND);
        r->guard.unlock(r->guard.context); return v;
    }
    return signal_condition(r,ea,target,0,report);
}
ps3_lwcond_result ps3_lwcond_cleanup(ps3_lwcond_registry *r,ps3_lwcond_report *report)
{
    if(!valid(r) || !report) return result(PS3_LWCOND_INVALID_ARGUMENT);
    r->guard.lock(r->guard.context); ps3_lwcond_result v=result(PS3_LWCOND_OK);
    for(size_t i=0;i<r->waiter_capacity;++i) if(r->waiters[i].state==CALL_ACTIVE) { v=result(PS3_LWCOND_BUSY); goto cleanup_done; }
    v=mutex_result(ps3_lwmutex_condition_cleanup(r->mutexes,&report->publications));
    if(v.status!=PS3_LWCOND_OK) { v.cleanup_retained=1; goto cleanup_done; }
    for(size_t i=0;i<r->waiter_capacity;++i) if(r->waiters[i].state==CALL_RETAINED) {
        if(r->waiters[i].reservation.registry) { v=result(PS3_LWCOND_CLEANUP_REQUIRED); v.cleanup_retained=v.must_not_resume=1; goto cleanup_done; }
        retire_waiter(r,i);
    }
    for(size_t i=0;i<r->object_capacity;++i) if(r->objects[i].state) {
        v=retire(r,i,report);
        if(v.status!=PS3_LWCOND_OK) { v.cleanup_retained=1; break; }
    }
cleanup_done:
    r->guard.unlock(r->guard.context); return v;
}
