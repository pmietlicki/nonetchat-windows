#pragma once
// Pure admission for the locally generated, single-audio Microsoft fMP4
// profile. No file, SDK, microphone, clock or network capability. ISO BMFF
// sizes/flags, Microsoft fragmented AAC output and FFmpeg 9.0.2
// mov_read_tfhd/mov_read_trun are the format references, never bitrate
// estimates.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "local_pcm_queue.h"

namespace nonet::fmp4 {
struct Bytes {
  const std::uint8_t* data;
  std::size_t size;
  bool Range(std::size_t offset, std::size_t count) const {
    return data && offset <= size && count <= size - offset;
  }
  std::optional<std::uint32_t> U32(std::size_t offset) const {
    if (!Range(offset, 4)) return {};
    const auto* p = data + offset;
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
  }
  std::optional<std::uint64_t> U64(std::size_t offset) const {
    const auto hi = U32(offset), lo = U32(offset + 4);
    if (!hi || !lo) return {};
    return (std::uint64_t(*hi) << 32) | *lo;
  }
};
constexpr std::uint32_t Tag(char a, char b, char c, char d) {
  return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) |
         (std::uint32_t(c) << 8) | std::uint32_t(d);
}
struct Box {
  std::size_t start, size, header;
  std::uint32_t kind;
};
inline std::optional<Box> At(Bytes bytes, std::size_t offset, std::size_t end) {
  if (end > bytes.size || offset > end || end - offset < 8) return {};
  const auto n = bytes.U32(offset), type = bytes.U32(offset + 4);
  if (!n || !type) return {};
  std::uint64_t size = *n;
  std::size_t header = 8;
  if (size == 1) {
    const auto large = bytes.U64(offset + 8);
    if (!large) return {};
    size = *large;
    header = 16;
  }
  // A zero-sized box depends on an open-ended EOF; it cannot reserve a
  // fragment.
  if (size < header || size > end - offset) return {};
  return Box{offset, static_cast<std::size_t>(size), header, *type};
}
inline std::optional<std::uint64_t> Add(std::uint64_t a, std::uint64_t b) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a) return {};
  return a + b;
}
inline std::optional<std::uint64_t> ScaleCeil(std::uint64_t ticks,
                                              std::uint32_t from,
                                              std::uint32_t to) {
  if (!from || !to) return {};
  const auto whole = ticks / from, rest = ticks % from;
  if (whole > std::numeric_limits<std::uint64_t>::max() / to) return {};
  // Both factors are below 2^32; this multiplication cannot overflow uint64.
  const auto fraction = rest * to;
  return Add(whole * to, fraction / from + (fraction % from ? 1 : 0));
}
struct Init {
  std::uint32_t movie_scale = 0, track_scale = 0, track_id = 0;
  std::uint64_t duration_offset = 0;
  std::size_t movie_offset = 0, movie_bytes = 0;
  bool wide_duration = false;
};
inline bool ParseMedia(Bytes b, Box mdia, Init& out) {
  bool clock = false, audio = false;
  for (auto p = mdia.start + mdia.header; p < mdia.start + mdia.size;) {
    const auto child = At(b, p, mdia.start + mdia.size);
    if (!child) return false;
    if (child->kind == Tag('m', 'd', 'h', 'd')) {
      if (clock || !b.Range(p + child->header, 4)) return false;
      const auto version = b.data[p + child->header];
      if (version > 1) return false;
      const auto at = p + child->header + (version ? 20 : 12);
      if (at + 4 > p + child->size) return false;
      const auto scale = b.U32(at);
      if (!scale || !*scale) return false;
      out.track_scale = *scale;
      clock = true;
    } else if (child->kind == Tag('h', 'd', 'l', 'r')) {
      if (audio) return false;
      if (child->size - child->header < 12) return false;
      const auto handler = b.U32(p + child->header + 8);
      if (!handler || *handler != Tag('s', 'o', 'u', 'n')) return false;
      audio = true;
    }
    p += child->size;
  }
  return clock && audio;
}
inline bool ParseTrack(Bytes b, Box trak, Init& out) {
  bool identity = false, media = false;
  for (auto p = trak.start + trak.header; p < trak.start + trak.size;) {
    const auto child = At(b, p, trak.start + trak.size);
    if (!child) return false;
    if (child->kind == Tag('t', 'k', 'h', 'd')) {
      if (identity || !b.Range(p + child->header, 4)) return false;
      const auto version = b.data[p + child->header];
      if (version > 1) return false;
      const auto at = p + child->header + (version ? 20 : 12);
      if (at + 4 > p + child->size) return false;
      const auto id = b.U32(at);
      if (!id || !*id) return false;
      out.track_id = *id;
      identity = true;
    } else if (child->kind == Tag('m', 'd', 'i', 'a')) {
      if (media || !ParseMedia(b, *child, out)) return false;
      media = true;
    }
    p += child->size;
  }
  return identity && media;
}
inline std::optional<Init> ParseInit(Bytes b, std::uint64_t absolute_offset) {
  const auto root = At(b, 0, b.size);
  if (!root || root->size != b.size || root->kind != Tag('m', 'o', 'o', 'v'))
    return {};
  Init out;
  bool movie = false, track = false;
  for (auto p = root->header; p < b.size;) {
    const auto child = At(b, p, b.size);
    if (!child) return {};
    if (child->kind == Tag('m', 'v', 'h', 'd')) {
      if (movie || !b.Range(p + child->header, 4)) return {};
      const auto version = b.data[p + child->header];
      if (version > 1) return {};
      const auto scale_at = p + child->header + (version ? 20 : 12);
      const auto duration_at = scale_at + 4;
      if (duration_at + (version ? 8 : 4) > p + child->size) return {};
      const auto scale = b.U32(scale_at);
      if (!scale || !*scale) return {};
      const auto location = Add(absolute_offset, duration_at);
      if (!location) return {};
      out.movie_scale = *scale;
      out.duration_offset = *location;
      out.movie_offset = p;
      out.movie_bytes = child->size;
      out.wide_duration = version != 0;
      movie = true;
    } else if (child->kind == Tag('t', 'r', 'a', 'k')) {
      if (track || !ParseTrack(b, *child, out)) return {};
      track = true;
    }
    p += child->size;
  }
  return movie && track ? std::optional<Init>(out) : std::nullopt;
}
struct Fragment {
  std::uint64_t ticks = 0, payload = 0, base = 0, samples = 0;
  std::int32_t data_offset = 0;
  bool location = false;
};
inline std::optional<Fragment> ParseFragment(Bytes b, const Init& init,
                                             std::uint64_t previous_ticks) {
  const auto root = At(b, 0, b.size);
  if (!root || root->size != b.size || root->kind != Tag('m', 'o', 'o', 'f'))
    return {};
  Fragment out;
  bool traf = false;
  for (auto p = root->header; p < b.size;) {
    const auto child = At(b, p, b.size);
    if (!child) return {};
    if (child->kind != Tag('t', 'r', 'a', 'f')) {
      p += child->size;
      continue;
    }
    if (traf) return {};
    traf = true;
    bool header = false, run = false, time = false;
    for (auto at = p + child->header; at < p + child->size;) {
      const auto box = At(b, at, p + child->size);
      if (!box) return {};
      const auto end = at + box->size, body = at + box->header;
      const auto has = [&](std::size_t from, std::size_t count) {
        return from <= end && count <= end - from;
      };
      if (box->kind == Tag('t', 'f', 'h', 'd')) {
        if (header || !has(body, 16)) return {};
        const auto flags = b.U32(body), track = b.U32(body + 4);
        const auto base = b.U64(body + 8);
        if (!flags || !track || !base || *track != init.track_id) return {};
        // This profile admits the observed Microsoft absolute
        // data base; other legal BMFF modes need explicit handling, never a
        // guess.
        if ((*flags & 0xffffff) != 1 || (*flags >> 24) != 0) return {};
        out.base = *base;
        header = true;
      } else if (box->kind == Tag('t', 'f', 'd', 't')) {
        if (time || !has(body, 4)) return {};
        time = true;
        const auto version = b.data[body];
        if (version > 1) return {};
        if (!has(body + 4, version ? 8 : 4)) return {};
        std::optional<std::uint64_t> stamp;
        if (version)
          stamp = b.U64(body + 4);
        else {
          const auto n = b.U32(body + 4);
          if (n) stamp = *n;
        }
        if (!stamp || *stamp != previous_ticks) return {};
      } else if (box->kind == Tag('t', 'r', 'u', 'n')) {
        if (run || !has(body, 8)) return {};
        run = true;
        const auto version_flags = b.U32(body), number = b.U32(body + 4);
        if (!version_flags || !number || !*number) return {};
        const auto flags = *version_flags & 0xffffff,
                   version = *version_flags >> 24;
        if (version > 1 || (flags & ~0xf05u) || !(flags & 0x100) ||
            !(flags & 0x200) || !(flags & 1))
          return {};
        auto cursor = body + 8;
        if (!has(cursor, 4)) return {};
        const auto offset = b.U32(cursor);
        if (!offset) return {};
        cursor += 4;
        // Avoid implementation-defined unsigned-to-signed conversion.
        out.data_offset =
            *offset <= 0x7fffffffu
                ? static_cast<std::int32_t>(*offset)
                : -1 - static_cast<std::int32_t>(0xffffffffu - *offset);
        out.location = true;
        if (flags & 4) {
          if (!has(cursor, 4)) return {};
          cursor += 4;
        }
        const std::size_t width =
            8 + (flags & 0x400 ? 4 : 0) + (flags & 0x800 ? 4 : 0);
        if (!has(cursor, 0) || *number > (end - cursor) / width) return {};
        for (std::uint32_t i = 0; i < *number; ++i) {
          const auto duration = b.U32(cursor), size = b.U32(cursor + 4);
          cursor += 8;
          if (!duration || !size || !*duration || !*size) return {};
          const auto ticks = Add(out.ticks, *duration),
                     bytes = Add(out.payload, *size);
          if (!ticks || !bytes) return {};
          out.ticks = *ticks;
          out.payload = *bytes;
          if (flags & 0x400) cursor += 4;
          if (flags & 0x800) {
            const auto composition = b.U32(cursor);
            if (!composition || *composition) return {};
            cursor += 4;
          }
        }
        if (cursor != end) return {};
        out.samples = *number;
      }
      at = end;
    }
    if (!header || !run || !out.location) return {};
    p += child->size;
  }
  return traf && out.ticks && out.payload ? std::optional<Fragment>(out)
                                          : std::nullopt;
}

enum class Stop { none, bytes, duration, queue, invalid, io, retired };
struct Plan {
  std::uint64_t generation = 0, begin = 0, end = 0, ticks = 0, samples = 0;
};
struct DurationPatch {
  std::uint64_t offset = 0, value = 0;
  std::size_t width = 0;
};
class AudioAdmission {
 public:
  // Existing FileTransferLayout.maxNativeBytes and Pixel Recorder limit.
  static constexpr std::uint64_t max_bytes = 2000000000ULL;
  static constexpr std::uint64_t max_seconds = 18ULL * 60 * 60;
  static constexpr std::size_t max_metadata_bytes =
      nonet::LocalPcmQueue::max_bytes;
  explicit AudioAdmission(std::uint64_t budget, std::size_t metadata_budget)
      : budget_(budget), metadata_budget_(metadata_budget) {
    if (!budget || budget > max_bytes || !metadata_budget ||
        metadata_budget > max_metadata_bytes)
      stop_ = Stop::invalid;
  }
  bool Initialize(Bytes moov, std::uint64_t absolute,
                  std::uint64_t physical_end) {
    if (stop_ != Stop::none || init_ || pending_) return false;
    if (moov.size > metadata_budget_) return Fail(Stop::queue);
    const auto end = Add(absolute, moov.size);
    if (!end || *end != physical_end || physical_end > budget_)
      return Fail(Stop::invalid);
    init_ = ParseInit(moov, absolute);
    if (!init_) return Fail(Stop::invalid);
    end_ = physical_end;
    return true;
  }
  std::optional<Plan> Reserve(Bytes moof, Bytes mdat_header,
                              std::uint64_t physical_end) {
    if (stop_ != Stop::none || !init_ || pending_) return {};
    if (physical_end != end_) {
      Fail(Stop::invalid);
      return {};
    }
    if (moof.size > metadata_budget_ ||
        mdat_header.size > metadata_budget_ - moof.size) {
      Fail(Stop::queue);
      return {};
    }
    if (mdat_header.size != 8 && mdat_header.size != 16) {
      Fail(Stop::invalid);
      return {};
    }
    const auto kind = mdat_header.U32(4), compact_size = mdat_header.U32(0);
    if (!kind || !compact_size || *kind != Tag('m', 'd', 'a', 't')) {
      Fail(Stop::invalid);
      return {};
    }
    std::optional<std::uint64_t> length;
    if (*compact_size == 1 && mdat_header.size == 16)
      length = mdat_header.U64(8);
    else if (*compact_size != 1 && mdat_header.size == 8)
      length = *compact_size;
    if (!length || *length < mdat_header.size) {
      Fail(Stop::invalid);
      return {};
    }
    const auto parsed = ParseFragment(moof, *init_, ticks_);
    if (!parsed || parsed->payload != *length - mdat_header.size) {
      Fail(Stop::invalid);
      return {};
    }
    auto location = parsed->base;
    if (parsed->data_offset < 0) {
      const auto distance = std::uint64_t(-(std::int64_t(parsed->data_offset)));
      if (distance > location) {
        Fail(Stop::invalid);
        return {};
      }
      location -= distance;
    } else {
      const auto sum =
          Add(location, static_cast<std::uint32_t>(parsed->data_offset));
      if (!sum) {
        Fail(Stop::invalid);
        return {};
      }
      location = *sum;
    }
    const auto data_begin = Add(physical_end, moof.size + mdat_header.size);
    if (!data_begin || location != *data_begin) {
      Fail(Stop::invalid);
      return {};
    }
    const auto prospective = Add(ticks_, parsed->ticks);
    if (!prospective) {
      Fail(Stop::invalid);
      return {};
    }
    if (*prospective > max_seconds * init_->track_scale) {
      Fail(Stop::duration);
      return {};
    }
    if (moof.size > budget_ - end_ || *length > budget_ - end_ - moof.size) {
      Fail(Stop::bytes);
      return {};
    }
    const auto end = Add(end_, moof.size + *length);
    if (!end) {
      Fail(Stop::invalid);
      return {};
    }
    const auto samples = Add(samples_, parsed->samples);
    if (!samples) {
      Fail(Stop::invalid);
      return {};
    }
    pending_ = Plan{++generation_, end_, *end, *prospective, *samples};
    return pending_;
  }
  bool Commit(const Plan& plan, std::uint64_t physical_end) {
    if (stop_ != Stop::none || !pending_ || !Same(plan, *pending_) ||
        physical_end != plan.end)
      return Fail(Stop::invalid);
    end_ = plan.end;
    ticks_ = plan.ticks;
    samples_ = plan.samples;
    ++fragments_;
    pending_.reset();
    return true;
  }
  bool Trailer(std::uint64_t begin, std::uint64_t bytes) {
    if (stop_ != Stop::none || pending_ || !init_ || begin != end_)
      return Fail(Stop::invalid);
    if (bytes > budget_ - end_) return Fail(Stop::bytes);
    return true;  // Caller owns/tracks its actual IO separately.
  }
  void ExternalLimit(Stop why) {
    if (!pending_ &&
        (why == Stop::bytes || why == Stop::duration || why == Stop::queue))
      Fail(why);
    else
      Fail(Stop::invalid);
  }
  void IoFailed() { stop_ = Stop::io; }
  void Retire() { stop_ = Stop::retired; }
  std::optional<DurationPatch> PrefixPatch(bool native_closed,
                                           bool expected_quota_failure) const {
    if (!native_closed || !expected_quota_failure || pending_ || !init_ ||
        !fragments_ || samples_ < 2 ||
        (stop_ != Stop::bytes && stop_ != Stop::duration &&
         stop_ != Stop::queue))
      return {};
    const auto value =
        ScaleCeil(ticks_, init_->track_scale, init_->movie_scale);
    if (!value || (!init_->wide_duration && *value > 0xffffffffu)) return {};
    return DurationPatch{init_->duration_offset, *value,
                         init_->wide_duration ? 8u : 4u};
  }
  Stop stop() const { return stop_; }
  std::uint64_t end() const { return end_; }
  std::uint64_t ticks() const { return ticks_; }
  std::uint64_t fragments() const { return fragments_; }

 private:
  static bool Same(const Plan& a, const Plan& b) {
    return a.generation == b.generation && a.begin == b.begin &&
           a.end == b.end && a.ticks == b.ticks && a.samples == b.samples;
  }
  bool Fail(Stop why) {
    if (stop_ == Stop::none) stop_ = why;
    return false;
  }
  const std::uint64_t budget_;
  const std::size_t metadata_budget_;
  Stop stop_ = Stop::none;
  std::optional<Init> init_;
  std::optional<Plan> pending_;
  std::uint64_t generation_ = 0, end_ = 0, ticks_ = 0, fragments_ = 0,
                samples_ = 0;
};
}  // namespace nonet::fmp4
