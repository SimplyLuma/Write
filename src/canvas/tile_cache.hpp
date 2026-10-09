#pragma once

#include "../engine/engine_types.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>

namespace luma::write::canvas {

struct TileKey {
  engine::Revision revision{0};
  int zoom_milli{1000};
  int device_scale{1};
  int part{0};
  engine::Rect document_area_twips;
  int pixel_width{0};
  int pixel_height{0};

  friend bool operator==(const TileKey&, const TileKey&) = default;
};

struct TileKeyHash {
  [[nodiscard]] std::size_t operator()(const TileKey& key) const noexcept;
};

[[nodiscard]] TileKey tile_key(const engine::TileRequest& request) noexcept;

class TileCache {
 public:
  explicit TileCache(std::size_t capacity_bytes);

  [[nodiscard]] std::shared_ptr<const engine::RenderedTile> find(const TileKey& key);
  void insert(TileKey key, std::shared_ptr<engine::RenderedTile> tile);
  void invalidate(const std::optional<engine::Rect>& area_twips,
                  engine::Revision current_revision);
  void clear() noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t bytes_used() const noexcept { return bytes_used_; }
  [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }

 private:
  struct Entry {
    TileKey key;
    std::shared_ptr<engine::RenderedTile> tile;
    std::size_t byte_size{0};
  };

  using RecencyList = std::list<Entry>;
  using Index = std::unordered_map<TileKey, RecencyList::iterator, TileKeyHash>;

  [[nodiscard]] static bool intersects(const engine::Rect& left,
                                       const engine::Rect& right) noexcept;
  void erase(RecencyList::iterator iterator);
  void trim();

  std::size_t capacity_bytes_{0};
  std::size_t bytes_used_{0};
  RecencyList recency_;
  Index entries_;
};

}  // namespace luma::write::canvas
