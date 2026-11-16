#ifndef PS3RECOMP_INSTANCE_GUARD_H
#define PS3RECOMP_INSTANCE_GUARD_H

/* Composition initializes and owns this nonrecursive guard. For a valid live
 * instance, callbacks are total, nonthrowing and not cancellation points.
 * Protected providers must not reenter the same core or introduce cancellation
 * points. Asynchronous host-thread cancellation is unsupported.
 * Nested cores/backing adapters must use distinct guards and one consistent
 * acyclic acquisition order; passing the same nonrecursive guard is invalid.
 * Callers release owned resources at cooperative boundaries and join before
 * freeing storage. */
typedef struct ps3_instance_guard {
    void *context;
    void (*lock)(void *);
    void (*unlock)(void *);
} ps3_instance_guard;

#endif
