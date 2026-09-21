// FCFN codec suite: replacement global allocation accounting.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "allocation_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::uint64_t> g_allocated_bytes{0};
std::atomic<std::uint64_t> g_allocation_calls{0};

void record(std::size_t size) noexcept {
  g_allocated_bytes.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
  g_allocation_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void* operator new(std::size_t size) {
  record(size);
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  return memory;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace fcfn::test {

std::uint64_t allocated_bytes() noexcept { return g_allocated_bytes.load(std::memory_order_relaxed); }
std::uint64_t allocation_calls() noexcept { return g_allocation_calls.load(std::memory_order_relaxed); }

}  // namespace fcfn::test
