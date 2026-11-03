/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Where a Direct3D 12 list's first timestamp is written: before the work
 * submitted ahead of it has drained, or after. The renderer times a frame
 * by a pair of EndQuery stamps, the first at the top of its list
 * (d3d12/frame.c); if that one is written as soon as the queue takes the
 * list up, while the previous frame's work is still running, the span
 * holds the end of that frame too, and fnl_frame_gpu_ms() reads high on
 * exactly the device that is busiest. Vulkan's start stamp was measured
 * against a bottom-of-pipe one and differed by 0.002 to 0.06 ms (DESIGN
 * *What a frame costs, compared*); this is the same question for D3D12.
 *
 * No window and no shader: list A copies a large buffer between two stamps,
 * list B starts with a stamp, copies a few bytes and stamps again, and the
 * four are resolved for reading. A is followed by a fence Signal and the
 * CPU waits on it, so the moment A was done is also known on the CPU's
 * clock; the queue's clock calibration puts the stamps on that clock too.
 * Two ways of submitting, each TRIALS times:
 *
 *   apart   A, Signal, B, Signal -- the renderer's way, one frame a submit
 *   joined  A and B in one ExecuteCommandLists, then one Signal
 *
 * and per trial the gap from A's end stamp to B's start stamp. Negative is
 * the answer that matters: B's start written before A had finished. The
 * CPU's wake on A's fence is an upper bound on A's end, and printed beside
 * it as a check of the calibration.
 *
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -D_WIN32_WINNT=0x0A00 \
 *      tools/probe/d3d12ts.c -ld3d12 -ldxgi -ldxguid -static -o d3d12ts.exe
 *   tools/probe/native_run.py d3d12ts.exe 60 Intel
 *   tools/probe/native_run.py d3d12ts.exe 60 NVIDIA
 *
 * The argument is any part of the adapter's name; with none, the first.
 * Exit status 0 when every trial was read, 1 when the device or a trial
 * failed.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRIALS   40
#define BIG      (128u << 20)   /* bytes A copies, each time */
#define COPIES   4              /* times A copies them: a few ms of work */
#define WAIT_MS  10000

#define RELEASE(p)                                                            \
	do {                                                                      \
		if (p) {                                                              \
			IUnknown *u_ = (IUnknown *)(void *)(p);                           \
			u_->lpVtbl->Release(u_);                                          \
			(p) = NULL;                                                       \
		}                                                                     \
	} while (0)

typedef struct {
	ID3D12Device              *dev;
	ID3D12CommandQueue        *queue;
	ID3D12CommandAllocator    *alloc[2];
	ID3D12GraphicsCommandList *list[2];
	ID3D12QueryHeap           *heap;
	ID3D12Resource            *src, *dst, *read;
	ID3D12Fence               *fence;
	HANDLE                     event;
	UINT64                     fence_value, ts_freq;
	LARGE_INTEGER              qpc_freq;
} Probe;

static bool ok(HRESULT hr, const char *what)
{
	if (SUCCEEDED(hr))
		return true;
	fprintf(stderr, "%s failed: 0x%08lx\n", what, (unsigned long)hr);
	return false;
}

static double qpc_ms(LONGLONG c, const Probe *p)
{
	return (double)c * 1e3 / (double)p->qpc_freq.QuadPart;
}

static IDXGIAdapter1 *pick_adapter(const char *want)
{
	IDXGIFactory4 *f = NULL;
	if (!ok(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&f),
			"CreateDXGIFactory2"))
		return NULL;
	IDXGIAdapter1 *found = NULL;
	for (UINT i = 0; !found; i++) {
		IDXGIAdapter1 *ad = NULL;
		if (IDXGIFactory4_EnumAdapters1(f, i, &ad) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 d;
		IDXGIAdapter1_GetDesc1(ad, &d);
		char name[256];
		WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name,
							sizeof name, NULL, NULL);
		fprintf(stderr, "adapter %u: %s\n", i, name);
		if (!want || strstr(name, want))
			found = ad;
		else
			RELEASE(ad);
	}
	RELEASE(f);
	return found;
}

static ID3D12Resource *buffer(Probe *p, D3D12_HEAP_TYPE type, UINT64 size,
							  D3D12_RESOURCE_STATES state)
{
	const D3D12_HEAP_PROPERTIES hp = { .Type = type };
	const D3D12_RESOURCE_DESC rd = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size,
		.Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
		.SampleDesc = { .Count = 1 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	ID3D12Resource *r = NULL;
	if (!ok(ID3D12Device_CreateCommittedResource(p->dev, &hp,
				D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource,
				(void **)&r), "CreateCommittedResource"))
		return NULL;
	return r;
}

static bool setup(Probe *p, IDXGIAdapter1 *ad)
{
	if (!ok(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0,
							  &IID_ID3D12Device, (void **)&p->dev),
			"D3D12CreateDevice"))
		return false;
	const D3D12_COMMAND_QUEUE_DESC qd = {
		.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
	};
	if (!ok(ID3D12Device_CreateCommandQueue(p->dev, &qd,
				&IID_ID3D12CommandQueue, (void **)&p->queue),
			"CreateCommandQueue"))
		return false;
	for (int i = 0; i < 2; i++) {
		if (!ok(ID3D12Device_CreateCommandAllocator(p->dev,
					D3D12_COMMAND_LIST_TYPE_DIRECT,
					&IID_ID3D12CommandAllocator, (void **)&p->alloc[i]),
				"CreateCommandAllocator") ||
			!ok(ID3D12Device_CreateCommandList(p->dev, 0,
					D3D12_COMMAND_LIST_TYPE_DIRECT, p->alloc[i], NULL,
					&IID_ID3D12GraphicsCommandList, (void **)&p->list[i]),
				"CreateCommandList"))
			return false;
		ID3D12GraphicsCommandList_Close(p->list[i]);
	}
	const D3D12_QUERY_HEAP_DESC hd = {
		.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 4,
	};
	if (!ok(ID3D12Device_CreateQueryHeap(p->dev, &hd, &IID_ID3D12QueryHeap,
										 (void **)&p->heap),
			"CreateQueryHeap"))
		return false;
	p->src = buffer(p, D3D12_HEAP_TYPE_DEFAULT, BIG,
					D3D12_RESOURCE_STATE_COMMON);
	p->dst = buffer(p, D3D12_HEAP_TYPE_DEFAULT, BIG,
					D3D12_RESOURCE_STATE_COMMON);
	p->read = buffer(p, D3D12_HEAP_TYPE_READBACK, 4 * sizeof(UINT64),
					 D3D12_RESOURCE_STATE_COPY_DEST);
	if (!p->src || !p->dst || !p->read)
		return false;
	if (!ok(ID3D12Device_CreateFence(p->dev, 0, D3D12_FENCE_FLAG_NONE,
									 &IID_ID3D12Fence, (void **)&p->fence),
			"CreateFence"))
		return false;
	p->event = CreateEventA(NULL, FALSE, FALSE, NULL);
	QueryPerformanceFrequency(&p->qpc_freq);
	return p->event &&
		   ok(ID3D12CommandQueue_GetTimestampFrequency(p->queue,
													   &p->ts_freq),
			  "GetTimestampFrequency");
}

/* A: stamp 0, the big copies, stamp 1. B: stamp 2, a few bytes, stamp 3,
 * and all four resolved. */
static bool record(Probe *p)
{
	ID3D12GraphicsCommandList *a = p->list[0], *b = p->list[1];
	if (!ok(ID3D12CommandAllocator_Reset(p->alloc[0]), "Reset") ||
		!ok(ID3D12CommandAllocator_Reset(p->alloc[1]), "Reset") ||
		!ok(ID3D12GraphicsCommandList_Reset(a, p->alloc[0], NULL), "Reset") ||
		!ok(ID3D12GraphicsCommandList_Reset(b, p->alloc[1], NULL), "Reset"))
		return false;
	ID3D12GraphicsCommandList_EndQuery(a, p->heap,
									   D3D12_QUERY_TYPE_TIMESTAMP, 0);
	for (int i = 0; i < COPIES; i++)
		ID3D12GraphicsCommandList_CopyBufferRegion(a, p->dst, 0, p->src, 0,
												   BIG);
	ID3D12GraphicsCommandList_EndQuery(a, p->heap,
									   D3D12_QUERY_TYPE_TIMESTAMP, 1);
	ID3D12GraphicsCommandList_EndQuery(b, p->heap,
									   D3D12_QUERY_TYPE_TIMESTAMP, 2);
	ID3D12GraphicsCommandList_CopyBufferRegion(b, p->dst, BIG - 256, p->src,
											   0, 256);
	ID3D12GraphicsCommandList_EndQuery(b, p->heap,
									   D3D12_QUERY_TYPE_TIMESTAMP, 3);
	ID3D12GraphicsCommandList_ResolveQueryData(b, p->heap,
		D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, p->read, 0);
	return ok(ID3D12GraphicsCommandList_Close(a), "Close") &&
		   ok(ID3D12GraphicsCommandList_Close(b), "Close");
}

/* The CPU's clock when the fence reached `v`, or 0 on a timeout. */
static LONGLONG wait_for(Probe *p, UINT64 v)
{
	if (ID3D12Fence_GetCompletedValue(p->fence) < v) {
		ID3D12Fence_SetEventOnCompletion(p->fence, v, p->event);
		if (WaitForSingleObject(p->event, WAIT_MS) != WAIT_OBJECT_0)
			return 0;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

typedef struct {
	double a_span, b_span, gap, a_end_vs_wake, b_start_vs_wake;
} Trial;

static bool trial(Probe *p, bool joined, Trial *t)
{
	if (!record(p))
		return false;
	UINT64 gpu0, cpu0;
	if (!ok(ID3D12CommandQueue_GetClockCalibration(p->queue, &gpu0, &cpu0),
			"GetClockCalibration"))
		return false;
	ID3D12CommandList *lists[2] = {
		(ID3D12CommandList *)p->list[0], (ID3D12CommandList *)p->list[1],
	};
	const UINT64 va = ++p->fence_value;
	UINT64 vb;
	if (joined) {
		ID3D12CommandQueue_ExecuteCommandLists(p->queue, 2, lists);
		ID3D12CommandQueue_Signal(p->queue, p->fence, va);
		vb = va;
	} else {
		ID3D12CommandQueue_ExecuteCommandLists(p->queue, 1, &lists[0]);
		ID3D12CommandQueue_Signal(p->queue, p->fence, va);
		ID3D12CommandQueue_ExecuteCommandLists(p->queue, 1, &lists[1]);
		vb = ++p->fence_value;
		ID3D12CommandQueue_Signal(p->queue, p->fence, vb);
	}
	const LONGLONG a_wake = wait_for(p, va);
	if (!a_wake || !wait_for(p, vb)) {
		fprintf(stderr, "a fence was not reached in %d ms\n", WAIT_MS);
		return false;
	}
	UINT64 *ts;
	const D3D12_RANGE all = { 0, 4 * sizeof(UINT64) }, none = { 0, 0 };
	if (!ok(ID3D12Resource_Map(p->read, 0, &all, (void **)&ts), "Map"))
		return false;
	UINT64 s[4];
	memcpy(s, ts, sizeof s);
	ID3D12Resource_Unmap(p->read, 0, &none);
	const double tick_ms = 1e3 / (double)p->ts_freq;
	/* A stamp on the CPU's clock, in ms, by the calibration taken before
	 * the submit: both clocks run on from that pair. */
	const double on_cpu1 = qpc_ms((LONGLONG)cpu0, p) +
						   (double)(int64_t)(s[1] - gpu0) * tick_ms;
	const double on_cpu2 = qpc_ms((LONGLONG)cpu0, p) +
						   (double)(int64_t)(s[2] - gpu0) * tick_ms;
	t->a_span = (double)(int64_t)(s[1] - s[0]) * tick_ms;
	t->b_span = (double)(int64_t)(s[3] - s[2]) * tick_ms;
	t->gap = (double)(int64_t)(s[2] - s[1]) * tick_ms;
	t->a_end_vs_wake = on_cpu1 - qpc_ms(a_wake, p);
	t->b_start_vs_wake = on_cpu2 - qpc_ms(a_wake, p);
	return true;
}

static int cmp(const void *x, const void *y)
{
	const double a = *(const double *)x, b = *(const double *)y;
	return (a > b) - (a < b);
}

static void summary(const char *what, double *v, int n)
{
	qsort(v, (size_t)n, sizeof *v, cmp);
	fprintf(stderr, "  %-34s min %8.3f  median %8.3f  max %8.3f ms\n", what,
			v[0], v[n / 2], v[n - 1]);
}

int main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IONBF, 0);
	IDXGIAdapter1 *ad = pick_adapter(argc > 1 ? argv[1] : NULL);
	if (!ad) {
		fprintf(stderr, "no adapter %s\n", argc > 1 ? argv[1] : "");
		return 1;
	}
	DXGI_ADAPTER_DESC1 d;
	IDXGIAdapter1_GetDesc1(ad, &d);
	char name[256];
	WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name,
						NULL, NULL);
	Probe p = { 0 };
	if (!setup(&p, ad))
		return 1;
	fprintf(stderr, "on %s, timestamps at %llu Hz, A copies %u MiB %d "
			"times\n", name, (unsigned long long)p.ts_freq, BIG >> 20,
			COPIES);
	int bad = 0;
	for (int joined = 0; joined < 2; joined++) {
		double a[TRIALS], b[TRIALS], g[TRIALS], ea[TRIALS], eb[TRIALS];
		int n = 0, negative = 0;
		for (int i = 0; i < TRIALS; i++) {
			Trial t;
			if (!trial(&p, joined != 0, &t)) {
				bad++;
				continue;
			}
			if (i < 3)
				fprintf(stderr, "%s %d: A %.3f ms, B %.3f, B's start - A's "
						"end %+.3f, A's end - A's fence wake %+.3f, B's "
						"start - A's fence wake %+.3f\n",
						joined ? "joined" : "apart", i, t.a_span, t.b_span,
						t.gap, t.a_end_vs_wake, t.b_start_vs_wake);
			a[n] = t.a_span;
			b[n] = t.b_span;
			g[n] = t.gap;
			ea[n] = t.a_end_vs_wake;
			eb[n] = t.b_start_vs_wake;
			negative += t.gap < 0.0;
			n++;
		}
		if (!n)
			continue;
		fprintf(stderr, "%s, %d trials, B's start before A's end in %d:\n",
				joined ? "joined (one ExecuteCommandLists)"
					   : "apart (a Signal between)", n, negative);
		summary("A, the big copies", a, n);
		summary("B, a few bytes", b, n);
		summary("B's start - A's end", g, n);
		summary("A's end - A's fence wake (CPU)", ea, n);
		summary("B's start - A's fence wake (CPU)", eb, n);
	}
	CloseHandle(p.event);
	RELEASE(p.fence);
	RELEASE(p.read);
	RELEASE(p.dst);
	RELEASE(p.src);
	RELEASE(p.heap);
	for (int i = 0; i < 2; i++) {
		RELEASE(p.list[i]);
		RELEASE(p.alloc[i]);
	}
	RELEASE(p.queue);
	RELEASE(p.dev);
	RELEASE(ad);
	return bad ? 1 : 0;
}
