#include "text-slice.h"
#include "text-buffer.h"
#include <cassert>
#include <vector>
#include "boost/regex.hpp"
#include <sstream>

using std::move;
using std::string;
using std::vector;

struct BaseLayer {
  const Text &text;

  BaseLayer(Text &text) : text{text} {}
  uint32_t size() const { return text.size(); }
  Point extent() const { return text.extent(); }
  uint16_t character_at(Point position) { return text.at(position); }
  ClipResult clip_position(Point position) { return text.clip_position(position); }

  template <typename Callback>
  bool for_each_chunk_in_range(Point start, Point end, const Callback &callback) {
    return callback(TextSlice(text).slice({start, end}));
  }
};

struct TextBuffer::Layer {
  union {
    Text *base_text;
    Layer *previous_layer;
  };

  Patch patch;
  Point extent_;
  uint32_t size_;
  uint32_t snapshot_count;
  bool is_first;
  bool is_last;

  Layer(Text *base_text) :
    base_text{base_text},
    extent_{base_text->extent()},
    size_{base_text->size()},
    snapshot_count{0},
    is_first{true},
    is_last{true} {}

  Layer(Layer *previous_layer) :
    previous_layer{previous_layer},
    extent_{previous_layer->extent()},
    size_{previous_layer->size()},
    snapshot_count{0},
    is_first{false},
    is_last{true} {}

  static inline Point previous_column(Point position) {
    return Point(position.row, position.column - 1);
  }

  template <typename T>
  uint16_t character_at_(T &previous_layer, Point position) {
    auto change = patch.find_change_for_new_position(position);
    if (!change) return previous_layer.character_at(position);
    if (position < change->new_end) {
      return change->new_text->at(position.traversal(change->new_start));
    } else {
      return previous_layer.character_at(
        change->old_end.traverse(position.traversal(change->new_end))
      );
    }
  }

  template <typename T>
  ClipResult clip_position_(T &previous_layer, Point position) {
    auto preceding_change = is_last ?
      patch.change_for_new_position(position) :
      patch.find_change_for_new_position(position);
    if (!preceding_change) return previous_layer.clip_position(position);

    uint32_t preceding_change_base_offset =
      previous_layer.clip_position(preceding_change->old_start).offset;
    uint32_t preceding_change_current_offset =
      preceding_change_base_offset +
      preceding_change->preceding_new_text_size -
      preceding_change->preceding_old_text_size;

    if (position < preceding_change->new_end) {
      ClipResult position_within_preceding_change =
        preceding_change->new_text->clip_position(
          position.traversal(preceding_change->new_start)
        );

      if (position_within_preceding_change.offset == 0 && preceding_change->old_start.column > 0) {
        if (preceding_change->new_text->content.front() == '\n' &&
            previous_layer.character_at(previous_column(preceding_change->old_start)) == '\r') {
          return {
            previous_column(preceding_change->new_start),
            preceding_change_current_offset - 1
          };
        }
      }

      return {
        preceding_change->new_start.traverse(position_within_preceding_change.position),
        preceding_change_current_offset + position_within_preceding_change.offset
      };
    } else {
      ClipResult base_location = previous_layer.clip_position(
        preceding_change->old_end.traverse(position.traversal(preceding_change->new_end))
      );

      ClipResult distance_past_preceding_change = {
        base_location.position.traversal(preceding_change->old_end),
        base_location.offset - (preceding_change_base_offset + preceding_change->old_text_size)
      };

      if (distance_past_preceding_change.offset == 0 && base_location.offset < previous_layer.size()) {
        uint16_t previous_character = 0;
        if (preceding_change->new_text->size() > 0) {
          previous_character = preceding_change->new_text->content.back();
        } else if (preceding_change->old_start.column > 0) {
          previous_character = previous_layer.character_at(previous_column(preceding_change->old_start));
        }

        if (previous_character == '\r' && previous_layer.character_at(base_location.position) == '\n') {
          return {
            previous_column(preceding_change->new_end),
            preceding_change_current_offset + preceding_change->new_text->size() - 1
          };
        }
      }

      return {
        preceding_change->new_end.traverse(distance_past_preceding_change.position),
        preceding_change_current_offset + preceding_change->new_text->size() + distance_past_preceding_change.offset
      };
    }
  }

  template <typename T, typename Callback>
  bool for_each_chunk_in_range_(T &previous_layer, Point start, Point end, const Callback &callback) {
    Point goal_position = clip_position(end).position;
    Point current_position = clip_position(start).position;
    Point base_position = current_position;
    auto change = patch.find_change_for_new_position(current_position);

    while (current_position < goal_position) {
      if (change) {
        if (current_position < change->new_end) {
          TextSlice slice = TextSlice(*change->new_text)
            .prefix(Point::min(
              goal_position.traversal(change->new_start),
              change->new_end.traversal(change->new_start)
            ))
            .suffix(current_position.traversal(change->new_start));
          if (callback(slice)) return true;
          base_position = change->old_end;
          current_position = change->new_end;
          if (current_position > goal_position) break;
        }

        base_position = change->old_end.traverse(current_position.traversal(change->new_end));
      }

      change = patch.find_change_ending_after_new_position(current_position);

      Point next_base_position, next_position;
      if (change) {
        next_position = Point::min(goal_position, change->new_start);
        next_base_position = Point::min(base_position.traverse(goal_position.traversal(current_position)), change->old_start);
      } else {
        next_position = goal_position;
        next_base_position = base_position.traverse(goal_position.traversal(current_position));
      }

      if (previous_layer.for_each_chunk_in_range(base_position, next_base_position, callback)) {
        return true;
      }
      base_position = next_base_position;
      current_position = next_position;
    }

    return false;
  }

  uint16_t character_at(Point position) {
    if (is_first) {
      BaseLayer base_layer(*base_text);
      return character_at_(base_layer, position);
    } else {
      return character_at_(*previous_layer, position);
    }
  }

  ClipResult clip_position(Point position) {
    if (is_first) {
      BaseLayer base_layer(*base_text);
      return clip_position_(base_layer, position);
    } else {
      return clip_position_(*previous_layer, position);
    }
  }

  Point position_for_offset(uint32_t goal_offset) {
    Point position;
    uint32_t offset = 0;

    for_each_chunk_in_range(Point(0, 0), extent(), [&position, &offset, goal_offset](TextSlice slice) {
      uint32_t size = slice.size();
      if (offset + size >= goal_offset) {
        position = position.traverse(slice.position_for_offset(goal_offset - offset));
        return true;
      }
      position = position.traverse(slice.extent());
      offset += size;
      return false;
    });

    return position;
  }

  template <typename Callback>
  bool for_each_chunk_in_range(Point start, Point end, const Callback &callback) {
    if (is_first) {
      BaseLayer base_layer(*base_text);
      return for_each_chunk_in_range_(base_layer, start, end, callback);
    } else {
      return for_each_chunk_in_range_(*previous_layer, start, end, callback);
    }
  }

  Point extent() const { return extent_; }

  uint32_t size() const { return size_; }

  void set_text_in_range(Range old_range, Text &&new_text) {
    auto start = clip_position(old_range.start);
    auto end = clip_position(old_range.end);
    Point new_range_end = start.position.traverse(new_text.extent());
    uint32_t deleted_text_size = end.offset - start.offset;
    extent_ = new_range_end.traverse(extent_.traversal(old_range.end));
    size_ += new_text.size() - deleted_text_size;
    patch.splice(
      old_range.start,
      old_range.extent(),
      new_text.extent(),
      optional<Text> {},
      move(new_text),
      deleted_text_size
    );
  }

  Text text_in_range(Range range) {
    Text result;
    for_each_chunk_in_range(range.start, range.end, [&result](TextSlice slice) {
      result.append(slice);
      return false;
    });
    return result;
  }

  vector<TextSlice> chunks_in_range(Range range) {
    vector<TextSlice> result;
    for_each_chunk_in_range(range.start, range.end, [&result](TextSlice slice) {
      result.push_back(slice);
      return false;
    });
    return result;
  }
};

TextBuffer::TextBuffer(Text &&text) :
  base_text{move(text)},
  top_layer{new TextBuffer::Layer(&this->base_text)} {}

TextBuffer::TextBuffer() :
  top_layer{new TextBuffer::Layer(&this->base_text)} {}

TextBuffer::~TextBuffer() { delete top_layer; }

TextBuffer::TextBuffer(std::u16string text) : TextBuffer {Text {text}} {}

bool TextBuffer::reset_base_text(Text &&new_base_text) {
  if (!top_layer->is_first) {
    return false;
  }

  top_layer->patch.clear();
  top_layer->extent_ = new_base_text.extent();
  top_layer->size_ = new_base_text.size();
  base_text = move(new_base_text);
  return true;
}

bool TextBuffer::flush_outstanding_changes() {
  if (!top_layer->is_first) return false;

  for (auto change : top_layer->patch.get_changes()) {
    base_text.splice(
      change.new_start,
      change.old_end.traversal(change.old_start),
      *change.new_text
    );
  }

  top_layer->patch.clear();
  return true;
}

bool TextBuffer::serialize_outstanding_changes(Serializer &serializer) {
  if (!top_layer->is_first) return false;
  top_layer->patch.serialize(serializer);
  serializer.append(top_layer->size_);
  top_layer->extent_.serialize(serializer);
  return true;
}

bool TextBuffer::deserialize_outstanding_changes(Deserializer &deserializer) {
  if (!top_layer->is_first || top_layer->patch.get_change_count() > 0) return false;
  top_layer->patch = Patch(deserializer);
  top_layer->size_ = deserializer.read<uint32_t>();
  top_layer->extent_ = Point(deserializer);
  return true;
}

template <typename T>
inline void hash_combine(std::size_t &seed, const T &value) {
    std::hash<T> hasher;
    seed ^= hasher(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

size_t TextBuffer::base_text_digest() {
  size_t result = 0;
  for (uint16_t character : base_text) {
    hash_combine(result, character);
  }
  return result;
}

Point TextBuffer::extent() const {
  return top_layer->extent();
}

uint32_t TextBuffer::size() const {
  return top_layer->size();
}

uint32_t TextBuffer::line_length_for_row(uint32_t row) {
  return top_layer->clip_position(Point{row, UINT32_MAX}).position.column;
}

const uint16_t *TextBuffer::line_ending_for_row(uint32_t row) {
  static uint16_t LF[] = {'\n', 0};
  static uint16_t CRLF[] = {'\r', '\n', 0};
  static uint16_t NONE[] = {0};

  const uint16_t *result = NONE;
  top_layer->for_each_chunk_in_range(
    Point(row, UINT32_MAX),
    Point(row + 1, 0),
    [&result](TextSlice slice) {
      auto begin = slice.begin();
      if (begin == slice.end()) return false;
      result = (*begin == '\r') ? CRLF : LF;
      return true;
    });
  return result;
}

ClipResult TextBuffer::clip_position(Point position) {
  return top_layer->clip_position(position);
}

Point TextBuffer::position_for_offset(uint32_t offset) {
  return top_layer->position_for_offset(offset);
}

Text TextBuffer::text() {
  return text_in_range(Range{Point(), extent()});
}

Text TextBuffer::text_in_range(Range range) {
  return top_layer->text_in_range(range);
}

vector<TextSlice> TextBuffer::chunks() const {
  return top_layer->chunks_in_range({{0, 0}, extent()});
}

void TextBuffer::set_text(Text &&new_text) {
  set_text_in_range(Range {Point(0, 0), extent()}, move(new_text));
}

void TextBuffer::set_text_in_range(Range old_range, Text &&new_text) {
  top_layer->set_text_in_range(old_range, move(new_text));
}

// Based on http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/p0169r0.html
class Iterator {
  const vector<TextSlice> *chunks;
  uint32_t chunk_index;
  Text::const_iterator chunk_begin;
  Text::const_iterator chunk_end;
  Text::const_iterator chunk_iterator;

  inline bool is_high_surrogate(uint16_t c) const {
    return (c & 0xdc00) == 0xd800;
  }

  inline bool is_low_surrogate(uint16_t c) const {
    return 0xdc00 <= c && c <= 0xdfff;
  }

  inline bool advance_chunk_if_needed() {
    while (chunk_iterator == chunk_end) {
      ++chunk_index;
      if (chunk_index < chunks->size()) {
        chunk_begin = chunks->at(chunk_index).begin();
        chunk_end = chunks->at(chunk_index).end();
        chunk_iterator = chunk_begin;
      } else {
        chunk_index = -1;
        chunk_begin = Text::const_iterator();
        chunk_end = Text::const_iterator();
        chunk_iterator = Text::const_iterator();
        return false;
      }
    }
    return true;
  }

public:
  using value_type = wchar_t;
  using pointer = wchar_t *;
  using reference = wchar_t &;
  using iterator_category = std::bidirectional_iterator_tag;
  using difference_type = Text::const_iterator::difference_type;

  Iterator() : chunk_index{static_cast<uint32_t>(-1)} {}

  Iterator(const vector<TextSlice> &chunks) :
    chunks{&chunks},
    chunk_index{0},
    chunk_begin{chunks[0].begin()},
    chunk_end{chunks[0].end()},
    chunk_iterator{chunk_begin} {}

  Iterator &operator++() {
    ++chunk_iterator;
    if (advance_chunk_if_needed() && is_high_surrogate(*chunk_iterator)) {
      ++chunk_iterator;
      advance_chunk_if_needed();
    }
    return *this;
  }

  Iterator &operator--() {
    if (chunk_iterator == chunk_begin) {
      if (chunk_index > 0) {
        chunk_index--;
        chunk_begin = chunks->at(chunk_index).begin();
        chunk_end = chunks->at(chunk_index).end();
        chunk_iterator = chunk_end - 1;
      }
    } else {
      --chunk_iterator;
    }

    if (chunk_iterator != chunk_begin && is_low_surrogate(*chunk_iterator)) {
      --chunk_iterator;
    }

    return *this;
  }

  wchar_t operator*() const {
    if (is_high_surrogate(*chunk_iterator)) {
      auto next_iterator = chunk_iterator;
      ++next_iterator;
      if (next_iterator != chunk_end) {
        return static_cast<wchar_t>(
          ((*chunk_iterator & 0x3ff) << 10 | (*next_iterator & 0x3ff)) + 0x10000
        );
      }
    }
    return static_cast<wchar_t>(*chunk_iterator);
  }

  bool operator!=(const Iterator &other) const {
    return !operator==(other);
  }

  bool operator==(const Iterator &other) const {
    if (chunk_index == static_cast<uint32_t>(-1)) {
      return other.chunk_index == static_cast<uint32_t>(-1);
    } else {
      return chunk_index == other.chunk_index && chunk_iterator == other.chunk_iterator;
    }
  }
};

namespace boost {
  void throw_exception(const std::exception &exception) {
    fprintf(stderr, "%s", exception.what());
    abort();
  }
}

int64_t TextBuffer::search(const std::string &pattern) const {
  std::wstring wpattern(pattern.begin(), pattern.end());
  boost::wregex regex;
  auto status = regex.set_expression(
    wpattern,
    boost::regex::ECMAScript|boost::regex_constants::no_except
  );
  if (status != 0) return INVALID_PATTERN;

  vector<TextSlice> chunks = this->chunks();
  boost::match_results<Iterator> match;
  if (boost::regex_search(Iterator(chunks), Iterator(), match, regex)) {
    return match.position();
  }

  return NO_RESULTS;
}

bool TextBuffer::is_modified() const {
  Layer *layer = top_layer;
  for (;;) {
    if (layer->patch.get_change_count() > 0) return true;
    if (layer->is_first) break;
    layer = layer->previous_layer;
  }
  return false;
}

string TextBuffer::get_dot_graph() const {
  Layer *layer = top_layer;
  vector<TextBuffer::Layer *> layers;
  for (;;) {
    layers.push_back(layer);
    if (layer->is_first) break;
    layer = layer->previous_layer;
  }

  std::stringstream result;
  result << "graph { label=\"--- buffer ---\" }\n";
  result << "graph { label=\"base:\n" << base_text << "\" }\n";
  for (auto begin = layers.rbegin(), iter = begin, end = layers.rend();
       iter != end; ++iter) {
    result << "graph { label=\"layer " << std::to_string(iter - begin) <<
      " (snapshot count " << std::to_string((*iter)->snapshot_count) <<
      "):\" }\n" << (*iter)->patch.get_dot_graph();
  }
  return result.str();
}

const TextBuffer::Snapshot *TextBuffer::create_snapshot() {
  Layer *layer;
  if (!top_layer->is_first && top_layer->patch.get_change_count() == 0) {
    layer = top_layer->previous_layer;
  } else {
    layer = top_layer;
    layer->is_last = false;
    top_layer = new Layer(top_layer);
  }
  layer->snapshot_count++;
  return new Snapshot(*this, *layer);
}

uint32_t TextBuffer::Snapshot::size() const {
  return layer.size();
}

Point TextBuffer::Snapshot::extent() const {
  return layer.extent();
}

uint32_t TextBuffer::Snapshot::line_length_for_row(uint32_t row) const {
  return layer.clip_position(Point{row, UINT32_MAX}).position.column;
}

Text TextBuffer::Snapshot::text_in_range(Range range) const {
  return layer.text_in_range(range);
}

Text TextBuffer::Snapshot::text() const {
  return layer.text_in_range({{0, 0}, extent()});
}

vector<TextSlice> TextBuffer::Snapshot::chunks_in_range(Range range) const {
  return layer.chunks_in_range(range);
}

vector<TextSlice> TextBuffer::Snapshot::chunks() const {
  return layer.chunks_in_range({{0, 0}, extent()});
}

TextBuffer::Snapshot::Snapshot(TextBuffer &buffer, TextBuffer::Layer &layer)
  : buffer{buffer}, layer{layer} {}

TextBuffer::Snapshot::~Snapshot() {
  assert(layer.snapshot_count > 0);
  layer.snapshot_count--;
  if (layer.snapshot_count > 0) return;

  // Find the topmost layer that has no snapshots pointing to it.
  vector<TextBuffer::Layer *> layers_to_remove;
  TextBuffer::Layer *top_layer = buffer.top_layer;
  if (top_layer->snapshot_count > 0) return;
  while (!top_layer->is_first && top_layer->previous_layer->snapshot_count == 0) {
    layers_to_remove.push_back(top_layer);
    top_layer = top_layer->previous_layer;
  }

  top_layer->size_ = buffer.top_layer->size_;
  top_layer->extent_ = buffer.top_layer->extent_;

  // Incorporate all the changes from upper layers into this layer.
  bool left_to_right = true;
  for (auto iter = layers_to_remove.rbegin(),
       end = layers_to_remove.rend();
       iter != end; ++iter) {
    top_layer->patch.combine((*iter)->patch, left_to_right);
    delete *iter;
    left_to_right = !left_to_right;
  }

  buffer.top_layer = top_layer;
  buffer.top_layer->is_last = true;
}
