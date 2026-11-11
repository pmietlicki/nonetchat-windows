#ifndef NONET_WINDOWS_MONITOR_AV_INDEX_H_
#define NONET_WINDOWS_MONITOR_AV_INDEX_H_
#include <functional>
#include <initializer_list>
#include <vector>

#include "fmp4_av_admission.h"
namespace nonet::av_inspection {
using namespace fmp4;
using namespace fmp4::av;
using Read = std::function<bool(std::uint64_t, std::uint8_t*, std::size_t)>;
struct Index {
  Initialization init;
  std::array<TrackState, 2> tracks{};
  std::uint32_t rate = 0, channels = 0;
  std::uint64_t bytes = 0, duration_us = 0;
};
inline std::optional<std::uint16_t> U16(Bytes b, std::size_t p) {
  if (!b.Range(p, 2)) return {};
  return static_cast<std::uint16_t>((std::uint16_t(b.data[p]) << 8) |
                                    b.data[p + 1]);
}
inline bool Children(Bytes b, Box root,
                     std::initializer_list<std::uint32_t> allowed,
                     bool tracks = false) {
  std::vector<std::uint32_t> seen;
  for (auto p = root.start + root.header; p < root.start + root.size;) {
    const auto child = At(b, p, root.start + root.size);
    if (!child) return false;
    if (std::find(allowed.begin(), allowed.end(), child->kind) == allowed.end())
      return false;
    if (std::find(seen.begin(), seen.end(), child->kind) != seen.end()) {
      if (!tracks || child->kind != Tag('t', 'r', 'a', 'k')) return false;
    } else
      seen.push_back(child->kind);
    p += child->size;
  }
  return true;
}
struct Descriptor {
  std::size_t payload, end;
};
inline std::optional<Descriptor> Desc(Bytes b, std::size_t& p, std::size_t end,
                                      std::uint8_t tag) {
  if (p >= end || end > b.size || b.data[p++] != tag) return {};
  std::size_t count = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (p >= end) return {};
    const auto byte = b.data[p++];
    count = (count << 7) | (byte & 127);
    if (!(byte & 128)) {
      if (count > end - p) return {};
      const Descriptor result{p, p + count};
      p += count;
      return result;
    }
  }
  return {};
}
class Bits {
 public:
  Bits(Bytes b, std::size_t begin, std::size_t end)
      : bytes_(b), bit_(begin * 8), end_(end * 8) {}
  std::optional<std::uint32_t> Take(unsigned n) {
    if (n > 32 || bit_ > end_ || n > end_ - bit_) return {};
    std::uint32_t out = 0;
    while (n--) {
      out = (out << 1) | ((bytes_.data[bit_ / 8] >> (7 - bit_ % 8)) & 1);
      ++bit_;
    }
    return out;
  }
  bool Padding() {
    while (bit_ < end_) {
      if (Take(1) != 0u) return false;
    }
    return true;
  }

 private:
  Bytes bytes_;
  std::size_t bit_, end_;
};
inline bool Aac(Bytes b, Box esds, std::uint32_t& rate,
                std::uint32_t& channels) {
  const auto body = esds.start + esds.header, end = esds.start + esds.size;
  if (!Has(esds, body, 4) || b.U32(body) != 0u) return false;
  auto p = body + 4;
  const auto es = Desc(b, p, end, 3);
  if (!es || p != end || es->end - es->payload < 3) return false;
  // No URL, stream dependency or OCR descriptor can reach the system parser.
  if (b.data[es->payload + 2] & 0xe0) return false;
  p = es->payload + 3;
  const auto decoder = Desc(b, p, es->end, 4);
  if (!decoder || decoder->end - decoder->payload < 13 ||
      b.data[decoder->payload] != 0x40 || b.data[decoder->payload + 1] != 0x15)
    return false;
  auto q = decoder->payload + 13;
  const auto config = Desc(b, q, decoder->end, 5);
  if (!config || q != decoder->end) return false;
  Bits bits(b, config->payload, config->end);
  if (bits.Take(5) != 2u) return false;
  const auto frequency = bits.Take(4);
  if (!frequency) return false;
  constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100,
                                     32000, 24000, 22050, 16000, 12000,
                                     11025, 8000,  7350};
  if (*frequency == 15) {
    const auto explicit_rate = bits.Take(24);
    if (!explicit_rate || !*explicit_rate) return false;
    rate = *explicit_rate;
  } else {
    if (*frequency >= 13) return false;
    rate = rates[*frequency];
  }
  const auto count = bits.Take(4);
  if (!count || (*count != 1 && *count != 2)) return false;
  channels = *count;
  // AAC-LC uses 1,024 decoded samples only under this checked GA config.
  if (bits.Take(3) != 0u || !bits.Padding()) return false;
  const auto sl = Desc(b, p, es->end, 6);
  return sl && p == es->end && sl->end - sl->payload == 1 &&
         b.data[sl->payload] == 2;
}
inline bool Avc(Bytes b, Box avcc) {
  const auto body = avcc.start + avcc.header, end = avcc.start + avcc.size;
  if (!Has(avcc, body, 7) || b.data[body] != 1 || b.data[body + 4] != 0xff ||
      (b.data[body + 5] & 0xe0) != 0xe0)
    return false;
  auto p = body + 6;
  const unsigned sps = b.data[body + 5] & 31;
  if (!sps) return false;
  for (unsigned i = 0; i < sps; ++i) {
    const auto n = U16(b, p);
    p += 2;
    if (!n || !*n || p > end || *n > end - p || (b.data[p] & 31) != 7)
      return false;
    p += *n;
  }
  if (p >= end) return false;
  const unsigned pps = b.data[p++];
  if (!pps) return false;
  for (unsigned i = 0; i < pps; ++i) {
    const auto n = U16(b, p);
    p += 2;
    if (!n || !*n || p > end || *n > end - p || (b.data[p] & 31) != 8)
      return false;
    p += *n;
  }
  // This local Microsoft VGA profile has no high-profile extension or metadata.
  return p == end;
}
inline bool StrictMoov(Bytes b, const Initialization& init, std::uint32_t& rate,
                       std::uint32_t& channels) {
  const auto root = At(b, 0, b.size);
  if (!root || !Children(b, *root,
                         {Tag('m', 'v', 'h', 'd'), Tag('t', 'r', 'a', 'k'),
                          Tag('m', 'v', 'e', 'x')},
                         true))
    return false;
  for (auto p = root->header; p < b.size;) {
    const auto trak = At(b, p, b.size);
    if (!trak) return false;
    p += trak->size;
    if (trak->kind != Tag('t', 'r', 'a', 'k')) continue;
    if (!Children(b, *trak, {Tag('t', 'k', 'h', 'd'), Tag('m', 'd', 'i', 'a')}))
      return false;
    const auto mdia = Child(b, *trak, Tag('m', 'd', 'i', 'a'));
    if (!mdia || !Children(b, *mdia,
                           {Tag('m', 'd', 'h', 'd'), Tag('h', 'd', 'l', 'r'),
                            Tag('m', 'i', 'n', 'f')}))
      return false;
    const auto minf = Child(b, *mdia, Tag('m', 'i', 'n', 'f'));
    if (!minf || !Children(b, *minf,
                           {Tag('v', 'm', 'h', 'd'), Tag('s', 'm', 'h', 'd'),
                            Tag('d', 'i', 'n', 'f'), Tag('s', 't', 'b', 'l')}))
      return false;
    const auto dinf = Child(b, *minf, Tag('d', 'i', 'n', 'f')),
               stbl = Child(b, *minf, Tag('s', 't', 'b', 'l'));
    if (!dinf || !Children(b, *dinf, {Tag('d', 'r', 'e', 'f')}) || !stbl ||
        !Children(b, *stbl,
                  {Tag('s', 't', 's', 'd'), Tag('s', 't', 't', 's'),
                   Tag('s', 't', 's', 'c'), Tag('s', 't', 's', 'z'),
                   Tag('s', 't', 'c', 'o'), Tag('s', 't', 's', 's')}))
      return false;
    for (auto at = stbl->start + stbl->header; at < stbl->start + stbl->size;) {
      const auto table = At(b, at, stbl->start + stbl->size);
      if (!table) return false;
      at += table->size;
      if (table->kind == Tag('s', 't', 's', 'd')) continue;
      const bool sizes = table->kind == Tag('s', 't', 's', 'z');
      if (table->size != table->header + (sizes ? 12 : 8) ||
          b.U32(table->start + table->header) != 0u ||
          b.U32(table->start + table->header + 4) != 0u ||
          (sizes && b.U32(table->start + table->header + 8) != 0u))
        return false;
    }
    const auto stsd = Child(b, *stbl, Tag('s', 't', 's', 'd'));
    if (!stsd) return false;
    const auto entry =
        At(b, stsd->start + stsd->header + 8, stsd->start + stsd->size);
    if (!entry) return false;
    const bool video = entry->kind == Tag('a', 'v', 'c', '1');
    const auto begin = entry->start + entry->header;
    std::optional<Box> config;
    for (auto at = begin + (video ? 78 : 28);
         at < entry->start + entry->size;) {
      const auto box = At(b, at, entry->start + entry->size);
      if (!box || config ||
          box->kind !=
              (video ? Tag('a', 'v', 'c', 'C') : Tag('e', 's', 'd', 's')))
        return false;
      config = box;
      at += box->size;
    }
    if (!config) return false;
    if (video) {
      if (U16(b, begin + 24) != 640u || U16(b, begin + 26) != 480u ||
          !Avc(b, *config))
        return false;
    } else {
      if (!Aac(b, *config, rate, channels) || rate > 65535 ||
          U16(b, begin + 16) != channels || U16(b, begin + 18) != 16u ||
          b.U32(begin + 24) != (rate << 16))
        return false;
    }
  }
  return rate && channels && init.tracks[0].video != init.tracks[1].video;
}
inline std::optional<std::uint64_t> FieldValue(Bytes moov, std::uint64_t start,
                                               DurationField field) {
  if (field.offset < start || field.offset - start > moov.size) return {};
  const auto at = static_cast<std::size_t>(field.offset - start);
  if (field.width == 4) return moov.U32(at);
  if (field.width == 8) return moov.U64(at);
  return {};
}
inline bool Prefix(Bytes b, std::uint32_t kind) {
  const auto root = At(b, 0, b.size);
  if (!root) return false;
  if (kind == Tag('f', 't', 'y', 'p')) {
    constexpr std::uint8_t expected[] = {
        0, 0, 0, 24, 'f', 't', 'y', 'p', 'm', 'p', '4', '2',
        0, 0, 0, 0,  'm', 'p', '4', '1', 'i', 's', 'o', 'm'};
    return b.size == sizeof(expected) &&
           !std::memcmp(b.data, expected, sizeof(expected));
  }
  if (kind == Tag('p', 'd', 'i', 'n'))
    return root->size == root->header + 12 && b.U32(root->header) == 0u;
  if (kind != Tag('u', 'u', 'i', 'd') || root->size < root->header + 20)
    return false;
  // Observed local-writer version UUID, followed only by its numeric label.
  constexpr std::uint8_t version[] = {0x5c, 0xa7, 0x08, 0xfb, 0x32, 0x8e,
                                      0x42, 0x05, 0xa8, 0x61, 0x65, 0x0e,
                                      0xca, 0x0a, 0x95, 0x96};
  const auto body = root->header;
  if (std::memcmp(b.data + body, version, 16)) return false;
  const auto size = b.U32(body + 16);
  if (!size || !*size || *size != b.size - body - 20) return false;
  return std::all_of(b.data + body + 20, b.data + b.size, [](std::uint8_t c) {
    return (c >= '0' && c <= '9') || c == '.';
  });
}
inline bool BoundedSamples(Bytes b) {
  const auto root = At(b, 0, b.size);
  if (!root) return false;
  for (auto p = root->header; p < b.size;) {
    const auto traf = At(b, p, b.size);
    if (!traf) return false;
    p += traf->size;
    if (traf->kind != Tag('t', 'r', 'a', 'f')) continue;
    const auto run = Child(b, *traf, Tag('t', 'r', 'u', 'n'));
    if (!run) continue;
    const auto at = run->start + run->header;
    const auto count = b.U32(at + 4);
    if (!count) return false;
    for (std::size_t n = 0; n < *count; ++n) {
      const auto bytes = b.U32(at + 12 + 16 * n + 4);
      if (!bytes || !*bytes || *bytes > LocalPcmQueue::max_bytes) return false;
    }
  }
  return true;
}
inline bool Trailer(Bytes b, const Initialization& init,
                    const std::array<TrackState, 2>& states,
                    std::uint64_t before) {
  const auto root = At(b, 0, b.size);
  if (!root || root->kind != Tag('m', 'f', 'r', 'a') || root->size != b.size)
    return false;
  std::array<bool, 2> seen{};
  bool footer = false;
  for (auto p = root->header; p < b.size;) {
    const auto box = At(b, p, b.size);
    if (!box || footer) return false;
    p += box->size;
    const auto at = box->start + box->header;
    if (box->kind == Tag('m', 'f', 'r', 'o')) {
      if (box->size != box->header + 8 || b.U32(at) != 0u ||
          b.U32(at + 4) != b.size || p != b.size)
        return false;
      footer = true;
      continue;
    }
    if (box->kind != Tag('t', 'f', 'r', 'a') || !Has(*box, at, 16) ||
        b.U32(at) != 0x01000000u)
      return false;
    const auto id = b.U32(at + 4), lengths = b.U32(at + 8),
               count = b.U32(at + 12);
    if (!id || !lengths || (*lengths & ~63u) || !count || !*count) return false;
    const auto i = *id == init.tracks[0].id   ? 0u
                   : *id == init.tracks[1].id ? 1u
                                              : 2u;
    if (i == 2 || seen[i]) return false;
    seen[i] = true;
    const std::array<unsigned, 3> fields{1 + ((*lengths >> 4) & 3),
                                         1 + ((*lengths >> 2) & 3),
                                         1 + (*lengths & 3)};
    const auto stride = 16 + fields[0] + fields[1] + fields[2];
    const auto left = box->start + box->size - at - 16;
    if (*count != left / stride || left % stride) return false;
    std::uint64_t previous_time = 0, previous_offset = 0;
    for (std::size_t n = 0; n < *count; ++n) {
      auto entry = at + 16 + n * stride;
      const auto time = b.U64(entry), offset = b.U64(entry + 8);
      if (!time || !offset || *time > states[i].decode_end || !*offset ||
          *offset >= before ||
          (n && (*time <= previous_time || *offset < previous_offset)))
        return false;
      entry += 16;
      for (const auto width : fields) {
        std::uint32_t value = 0;
        for (unsigned j = 0; j < width; ++j)
          value = (value << 8) | b.data[entry++];
        if (!value) return false;
      }
      previous_time = *time;
      previous_offset = *offset;
    }
  }
  return footer && seen[0] && seen[1];
}
inline std::optional<Index> Inspect(std::uint64_t size, const Read& read) {
  if (!size || size > AudioAdmission::max_bytes || !read) return {};
  try {
    Admission admission(size, Admission::metadata_limit, 0, 0,
                        FragmentInput::stored_cts);
    Index index;
    index.bytes = size;
    std::array<std::uint64_t, 5> declared{};
    std::vector<std::uint8_t> moof;
    bool initialized = false, ftyp = false, trailer = false;
    std::array<bool, 3> prefixes{};
    std::uint64_t at = 0, payload_end = size;
    while (at < size) {
      std::uint8_t header[16]{};
      if (size - at < 8 || !read(at, header, 8)) return {};
      std::size_t head = 8;
      Bytes bytes{header, 8};
      const auto short_size = bytes.U32(0), kind = bytes.U32(4);
      if (!short_size || !*short_size || !kind) return {};
      std::uint64_t length = *short_size;
      if (length == 1) {
        if (size - at < 16 || !read(at + 8, header + 8, 8)) return {};
        head = 16;
        bytes.size = 16;
        const auto wide = bytes.U64(8);
        if (!wide) return {};
        length = *wide;
      }
      if (length < head || length > size - at || trailer) return {};
      if (*kind == Tag('m', 'd', 'a', 't')) {
        if (moof.empty()) return {};
        const auto plan = admission.Reserve(moof.data(), moof.size(),
                                            {header, head}, at - moof.size());
        if (!plan || plan->end != at + length ||
            !BoundedSamples({moof.data(), moof.size()}) ||
            !admission.Commit(*plan, plan->end))
          return {};
        moof.clear();
        at += length;
        continue;
      }
      if (length > Admission::metadata_limit ||
          (!moof.empty() && *kind != Tag('m', 'd', 'a', 't')))
        return {};
      std::vector<std::uint8_t> block(static_cast<std::size_t>(length));
      if (!read(at, block.data(), block.size())) return {};
      const Bytes body{block.data(), block.size()};
      if (*kind == Tag('m', 'o', 'o', 'v')) {
        if (initialized || !ftyp ||
            !admission.Initialize(body, at, at + length))
          return {};
        index.init = *admission.initialization();
        if (!StrictMoov(body, index.init, index.rate, index.channels))
          return {};
        const auto movie = FieldValue(body, at, index.init.movie_duration);
        if (!movie) return {};
        declared[0] = *movie;
        for (std::size_t i = 0; i < 2; ++i) {
          const auto td = FieldValue(body, at,
                                     index.init.tracks[i].track_duration),
                     md = FieldValue(body, at,
                                     index.init.tracks[i].media_duration);
          if (!td || !md) return {};
          declared[1 + 2 * i] = *td;
          declared[2 + 2 * i] = *md;
        }
        initialized = true;
      } else if (*kind == Tag('m', 'o', 'o', 'f')) {
        if (!initialized || at != admission.end()) return {};
        moof = std::move(block);
      } else if (*kind == Tag('m', 'f', 'r', 'a')) {
        if (!initialized ||
            !Trailer(body, index.init, admission.tracks(), at) ||
            !admission.Trailer(at, length))
          return {};
        payload_end = at;
        trailer = true;
      } else {
        if (initialized) return {};
        const auto slot = *kind == Tag('f', 't', 'y', 'p')   ? 0u
                          : *kind == Tag('u', 'u', 'i', 'd') ? 1u
                          : *kind == Tag('p', 'd', 'i', 'n') ? 2u
                                                             : 3u;
        if (slot == 3 || prefixes[slot] || (!ftyp && slot != 0) ||
            !Prefix(body, *kind))
          return {};
        prefixes[slot] = true;
        if (slot == 0) ftyp = true;
      }
      at += length;
    }
    if (!initialized || !moof.empty() || admission.end() != payload_end)
      return {};
    const auto patches =
        admission.ClosedPatches(true, false, index.init.movie_scale);
    if (!patches) return {};
    for (std::size_t i = 0; i < declared.size(); ++i)
      if ((*patches)[i].value != declared[i]) return {};
    index.tracks = admission.tracks();
    const auto seconds = declared[0] / index.init.movie_scale;
    if (seconds > AudioAdmission::max_seconds) return {};
    index.duration_us =
        seconds * 1000000 + (declared[0] % index.init.movie_scale) * 1000000 /
                                index.init.movie_scale;
    if (!index.duration_us ||
        index.duration_us > AudioAdmission::max_seconds * 1000000)
      return {};
    return index;
  } catch (...) {
    return {};
  }
}
}  // namespace nonet::av_inspection
#endif
