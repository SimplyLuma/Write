#include "tile_cache.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace luma::write::canvas {
namespace {

void hash_combine(std::size_t& seed, const std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

}  // namespace

std::size_t TileKeyHash::operator()(const TileKey& key) const noexcept {
  std::size_t seed = 0;
  const std::hash<std::int64_t> signed_hash;
  const std::hash<std::uint64_t> unsigned_hash;
  hash_combine(seed, unsigned_hash(key.revision));
  hash_combine(seed, signed_hash(key.zoom_milli));
  hash_combine(seed, signed_hash(key.device_scale));
  hash_combine(seed, signed_hash(key.part));
  hash_combine(seed, signed_hash(key.document_area_twips.x));
  hash_combine(seed, signed_hash(key.document_area_twips.y));
  hash_combine(seed, signed_hash(key.document_area_twips.width));
  hash_combine(seed, signed_hash(key.document_area_twips.height));
  hash_combine(seed, signed_hash(key.pixel_width));
  hash_combine(seed, signed_hash(key.pixel_height));
  return seed;
}

TileKey tile_key(const engine::TileRequest& request) noexcept {
  return {.revision = request.revision,
          .zoom_milli = static_cast<int>(std::lround(request.zoom * 1000.0)),
          .device_scale = request.device_scale,
          .part = request.part,
          .document_area_twips = request.document_area_twips,
          .pixel_width = request.pixel_width,
          .pixel_height = request.pixel_height};
}

TileCache::TileCache(const std::size_t capacity_bytes)
    : capacity_bytes_(std::max<std::size_t>(capacity_bytes, 1U)) {}

std::shared_ptr<const engine::RenderedTile> TileCache::find(const TileKey& key) {
  const auto match = entries_.find(key);
  if (match == entries_.end()) {
    return {};
  }
  recency_.splice(recency_.begin(), recency_, match->second);
  return match->second->tile;
}

void TileCache::insert(TileKey key, std::shared_ptr<engine::RenderedTile> tile) {
  if (!tile) {
    return;
  }
  if (const auto existing = entries_.find(key); existing != entries_.end()) {
    erase(existing->second);
  }
  const auto byte_size = tile->pixels.size();
  recency_.push_front({std::move(key), std::move(tile), byte_size});
  entries_.emplace(recency_.front().key, recency_.begin());
  bytes_used_ += byte_size;
  trim();
}

bool TileCache::intersects(const engine::Rect& left, const engine::Rect& right) noexcept {
  if (left.empty() || right.empty()) {
    return false;
  }
  return left.x < right.x + right.width && right.x < left.x + left.width &&
         left.y < right.y + right.height && right.y < left.y + left.height;
}

void TileCache::invalidate(const std::optional<engine::Rect>& area_twips,
                           const engine::Revision current_revision) {
  for (auto iterator = recency_.begin(); iterator != recency_.end();) {
    const bool stale_revision = iterator->key.revision != current_revision;
    const bool overlaps = area_twips.has_value() &&
                          intersects(iterator->key.document_area_twips, *area_twips);
    if (stale_revision || overlaps || !area_twips.has_value()) {
      auto doomed = iterator++;
      erase(doomed);
    } else {
      ++iterator;
    }
  }
}

void TileCache::clear() noexcept {
  entries_.clear();
  recency_.clear();
  bytes_used_ = 0;
}

void TileCache::erase(const RecencyList::iterator iterator) {
  bytes_used_ -= std::min(bytes_used_, iterator->byte_size);
  entries_.erase(iterator->key);
  recency_.erase(iterator);
}

void TileCache::trim() {
  while (bytes_used_ > capacity_bytes_ && recency_.size() > 1) {
    erase(std::prev(recency_.end()));
  }
}

}  // namespace luma::write::canvas
