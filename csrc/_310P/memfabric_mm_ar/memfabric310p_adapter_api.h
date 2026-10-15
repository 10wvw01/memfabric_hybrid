/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable internal ABI between vLLM-Ascend and the installed 310P MemFabric
 * package. ABI v7 is batch based: MemFabric remains an opaque transport and
 * only public host APIs plus public device signal/wait/quiet are used.
 */
#define VLLM_ASCEND_MF310P_ADAPTER_ABI_VERSION 7u
#define VLLM_ASCEND_MF310P_MAX_BATCHES 64u
#define VLLM_ASCEND_MF310P_COOPERATIVE_CORES 8u

typedef struct mf310p_context mf310p_context_t;

typedef struct mf310p_layout {
    uint64_t pool_base;
    uint64_t own_segment;
    uint64_t peer_segment;
    uint64_t symmetric_size;
    uint64_t local_size;

    /* vLLM-owned application data inside each symmetric rank segment. */
    uint64_t send_arena;
    uint64_t recv_arena;
    uint64_t peer_recv_arena;
    uint64_t ack_slot;
    uint64_t peer_ack_slot;

    uint64_t arena_bytes;
    uint64_t batch_bytes;
    uint64_t expected_credit_dst;
    uint64_t expected_recv_base;

    uint32_t arena_rows;
    uint32_t batch_m;
    uint32_t max_batches;
    uint32_t reserved;
} mf310p_layout_t;

uint32_t mf310p_adapter_abi_version(void);

int mf310p_create(
    int rank,
    int world_size,
    const char* store_url,
    uint64_t local_size,
    uint32_t arena_rows,
    uint32_t batch_m,
    mf310p_context_t** out_ctx);

int mf310p_destroy(mf310p_context_t* ctx);
int mf310p_get_layout(mf310p_context_t* ctx, mf310p_layout_t* out_layout);
int mf310p_debug_protocol_status(
    mf310p_context_t* ctx,
    uint64_t* out_status);

/* One-time host rendezvous during protocol initialization only. */
int mf310p_control_barrier(mf310p_context_t* ctx);

/*
 * One-time per-process GVA geometry exchange over the public control network.
 * Mail dst fields carry sender-space GVAs, so receiver validation uses the
 * peer mapping learned here.
 */
int mf310p_exchange_geometry(mf310p_context_t* ctx);

/*
 * Clear vLLM-owned per-wave protocol state (the 64 B error channel is always
 * cleared). The 32 KiB ready-cell region is only cleared when the producer
 * generation is wave-invariant (graph capture); eager waves use a monotonic
 * generation that cannot equal a stale cell value and pass 0 here.
 */
int mf310p_prepare_wave_async(
    mf310p_context_t* ctx,
    uint32_t clear_wave_control,
    void* acl_stream);

/* Seed exactly one fixed wave credit and quiet it. */
int mf310p_init_credit_async(mf310p_context_t* ctx, void* acl_stream);

/*
 * One batch producer launch. All eight AI cores participate in the same
 * cooperative MM. Core0 is also the sole data signal owner after every core
 * publishes ready == generation.
 */
int mf310p_direct_producer_async(
    mf310p_context_t* ctx,
    uint64_t x,
    uint64_t w,
    uint32_t batch_index,
    uint32_t generation,
    void* acl_stream);

/*
 * Consume and strictly validate exactly one peer data mail for batch_index.
 * quiet() is deliberately not part of this hot-path call.
 */
int mf310p_wait_batch_async(
    mf310p_context_t* ctx,
    uint32_t batch_index,
    void* acl_stream);

int mf310p_warmup_producer_async(
    mf310p_context_t* ctx,
    void* acl_stream);

int mf310p_warmup_waiter_async(mf310p_context_t* ctx, void* acl_stream);

/*
 * Reduce one batch from send/recv arenas into out. valid_rows may be smaller
 * than batch_m only for the final padded batch.
 */
int mf310p_add_batch_async(
    mf310p_context_t* ctx,
    uint64_t out,
    uint32_t batch_index,
    uint32_t valid_rows,
    void* acl_stream);

int mf310p_warmup_add_async(mf310p_context_t* ctx, void* acl_stream);

/* ---- Small-M exact path (N-split, M < batch_m) ---- */

/*
 * One-time per-layer weight slice build: re-layout the full [2048, 2048]
 * NZ weight into eight contiguous per-core [256, 2048] NZ column slices
 * (slice-major layout, 8 MiB). Runs as strided device-to-device copies on
 * the caller's stream; the caller owns the output buffer.
 */
int mf310p_build_weight_slices(
    mf310p_context_t* ctx,
    uint64_t weight_nz,
    uint64_t slices_out,
    void* acl_stream);

/*
 * One small-wave N-split producer launch: all eight AI cores compute the
 * staged [T, 2048] window against their own 256-column slice into the
 * blocked [8][T][256] batch-0 slot; core0 signals the whole t_rows*2048*2
 * byte slot to the peer after the ready rendezvous. t_rows is the template
 * stair {16, 32, 64, 128, 256} covering valid_rows.
 */
int mf310p_small_producer_async(
    mf310p_context_t* ctx,
    uint64_t x_staging,
    uint64_t weight_slices,
    uint32_t valid_rows,
    uint32_t t_rows,
    uint32_t generation,
    void* acl_stream);

/*
 * Wait the single small-path mail: batch 0 with expected length
 * t_rows * 2048 * 2 bytes (the existing wait kernel is reused verbatim
 * through its batchBytes argument).
 */
int mf310p_wait_small_async(
    mf310p_context_t* ctx,
    uint32_t t_rows,
    void* acl_stream);

/* Unblocking reduce of exactly valid_rows rows from the blocked slot. */
int mf310p_add_small_async(
    mf310p_context_t* ctx,
    uint64_t out,
    uint32_t valid_rows,
    uint32_t t_rows,
    void* acl_stream);

int mf310p_warmup_nsplit_producer_async(
    mf310p_context_t* ctx,
    void* acl_stream);

int mf310p_warmup_add_blocked_async(mf310p_context_t* ctx, void* acl_stream);

/* Wave-level fixed credit. */
int mf310p_ack_async(mf310p_context_t* ctx, void* acl_stream);
int mf310p_gate_async(mf310p_context_t* ctx, void* acl_stream);
int mf310p_quiet_async(mf310p_context_t* ctx, void* acl_stream);

int mf310p_warmup_protocol_async(mf310p_context_t* ctx, void* acl_stream);

#ifdef __cplusplus
}
#endif
