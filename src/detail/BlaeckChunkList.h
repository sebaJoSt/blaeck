#pragma once

#include <stdint.h>
#include <new>

namespace blaeck
{
namespace detail
{
// A table that grows by whole chunks of N entries as it fills. A chunk, once allocated, is
// never moved or freed until the table is destroyed, so the heap doesn't fragment: entries are
// added in setup(), and their chunks stack up without gaps. A cleared table keeps its chunks
// and reuses them.
//
// Indexing walks the chunks. It starts from the chunk used last when that lies on the way, so
// a loop over the table in order costs one step per chunk, not per entry.
template <class T, uint8_t N = 8>
class ChunkList
{
public:
  ChunkList() {}
  ~ChunkList()
  {
    Chunk *c = _head;
    while (c != nullptr)
    {
      Chunk *next = c->next;
      delete c;
      c = next;
    }
  }
  ChunkList(const ChunkList &) = delete;
  ChunkList &operator=(const ChunkList &) = delete;

  // Entries that exist: the allocated chunks times N. Index only below this.
  uint16_t capacity() const { return _capacity; }

  // Grows until at least count entries exist. False if RAM ran out; the entries already
  // there stay.
  bool reserve(uint16_t count)
  {
    while (_capacity < count)
    {
      Chunk *c = new (std::nothrow) Chunk();
      if (c == nullptr)
        return false;
      if (_tail != nullptr)
        _tail->next = c;
      else
        _head = c;
      _tail = c;
      _capacity += N;
    }
    return true;
  }

  // The entry at i, which must be below capacity(). Const, like indexing a pointer: the
  // entries are not part of the table's own state.
  T &operator[](uint16_t i) const
  {
    if (_cursor == nullptr || i < _cursorBase)
    {
      _cursor = _head;
      _cursorBase = 0;
    }
    while (i >= _cursorBase + N)
    {
      _cursor = _cursor->next;
      _cursorBase += N;
    }
    return _cursor->items[i - _cursorBase];
  }

private:
  struct Chunk
  {
    T items[N];
    Chunk *next = nullptr;
  };

  Chunk *_head = nullptr;
  Chunk *_tail = nullptr;
  mutable Chunk *_cursor = nullptr;
  mutable uint16_t _cursorBase = 0;
  uint16_t _capacity = 0;
};
} // namespace detail
} // namespace blaeck
