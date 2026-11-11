#pragma once
// Prototype only: admission of the exact, newly generated Microsoft AAC/H264
// fragmented profile. No IO, receiver, path, clock sampling or playback right.
// The caller supplies both starts from one already-owned continuous epoch.
// Native MF normalizes each track independently. Preserve that original gap in
// each trun's standard composition field without changing any box/payload size.
#include <algorithm>
#include <array>
#include <cstring>

#include "fmp4_audio_admission.h"

namespace nonet::fmp4::av {
struct DurationField {
  std::uint64_t offset = 0;
  std::size_t width = 0;
};
struct Track {
  std::uint32_t id = 0, scale = 0;
  bool video = false;
  DurationField track_duration, media_duration;
};
struct Initialization {
  std::uint32_t movie_scale = 0;
  DurationField movie_duration;
  std::array<Track, 2> tracks{};
};
inline bool Has(Box box, std::size_t from, std::size_t count) {
  return from >= box.start + box.header && from <= box.start + box.size &&
         count <= box.start + box.size - from;
}
inline std::optional<Box> Child(Bytes b, Box parent, std::uint32_t kind) {
  std::optional<Box> result;
  for (auto p = parent.start + parent.header; p < parent.start + parent.size;) {
    const auto box = At(b, p, parent.start + parent.size);
    if (!box) return {};
    if (box->kind == kind) {
      if (result) return {};
      result = box;
    }
    p += box->size;
  }
  return result;
}
inline bool ContainsOrMalformed(Bytes b, Box parent, std::uint32_t kind) {
  for (auto p = parent.start + parent.header; p < parent.start + parent.size;) {
    const auto box = At(b, p, parent.start + parent.size);
    if (!box || box->kind == kind) return true;
    p += box->size;
  }
  return false;
}
inline bool Duration(Bytes b, Box box, std::uint64_t absolute, bool identity,
                     std::uint32_t& value, DurationField& field) {
  const auto body = box.start + box.header;
  if (!Has(box, body, 4)) return false;
  const auto version = b.data[body];
  if (version > 1) return false;
  const auto at = body + (version ? 20 : 12);
  const auto duration = at + (identity ? 8 : 4);
  if (!Has(box, at, 4) || !Has(box, duration, version ? 8 : 4)) return false;
  const auto scalar = b.U32(at);
  const auto location = Add(absolute, duration);
  if (!scalar || !*scalar || !location) return false;
  value = *scalar;
  field = {*location, version ? 8u : 4u};
  return true;
}
inline bool SelfContained(Bytes b, Box minf, bool video) {
  const auto dinf = Child(b, minf, Tag('d', 'i', 'n', 'f'));
  const auto stbl = Child(b, minf, Tag('s', 't', 'b', 'l'));
  if (!dinf || !stbl) return false;
  const auto dref = Child(b, *dinf, Tag('d', 'r', 'e', 'f'));
  const auto stsd = Child(b, *stbl, Tag('s', 't', 's', 'd'));
  if (!dref || !stsd) return false;
  const auto db = dref->start + dref->header;
  const auto sb = stsd->start + stsd->header;
  if (!Has(*dref, db, 8) || !Has(*stsd, sb, 8) || b.U32(db) != 0u ||
      b.U32(db + 4) != 1u || b.U32(sb) != 0u || b.U32(sb + 4) != 1u)
    return false;
  const auto url = At(b, db + 8, dref->start + dref->size);
  if (!url || url->kind != Tag('u', 'r', 'l', ' ') ||
      url->size != url->header + 4 ||
      url->start + url->size != dref->start + dref->size ||
      b.U32(url->start + url->header) != 1u)
    return false;
  const auto entry = At(b, sb + 8, stsd->start + stsd->size);
  if (!entry ||
      entry->kind !=
          (video ? Tag('a', 'v', 'c', '1') : Tag('m', 'p', '4', 'a')) ||
      entry->start + entry->size != stsd->start + stsd->size)
    return false;
  const auto body = entry->start + entry->header;
  const std::size_t header = video ? 78 : 28;
  if (!Has(*entry, body, header) || b.data[body + 6] || b.data[body + 7] != 1)
    return false;
  // Require exactly one bounded native codec configuration; other entries are
  // deliberately outside this local-writer profile, never an old-file reader.
  bool codec = false;
  for (auto p = body + header; p < entry->start + entry->size;) {
    const auto box = At(b, p, entry->start + entry->size);
    if (!box) return false;
    if (box->kind ==
        (video ? Tag('a', 'v', 'c', 'C') : Tag('e', 's', 'd', 's'))) {
      if (codec || box->size <= box->header) return false;
      codec = true;
    }
    p += box->size;
  }
  return codec;
}
inline std::optional<Initialization> Parse(Bytes b, std::uint64_t absolute) {
  const auto root = At(b, 0, b.size);
  if (!root || root->kind != Tag('m', 'o', 'o', 'v') || root->size != b.size)
    return {};
  const auto mvhd = Child(b, *root, Tag('m', 'v', 'h', 'd'));
  const auto mvex = Child(b, *root, Tag('m', 'v', 'e', 'x'));
  if (!mvhd || !mvex) return {};
  Initialization result;
  if (!Duration(b, *mvhd, absolute, false, result.movie_scale,
                result.movie_duration))
    return {};
  std::size_t count = 0;
  for (auto p = root->header; p < b.size;) {
    const auto box = At(b, p, b.size);
    if (!box) return {};
    if (box->kind == Tag('t', 'r', 'a', 'k')) {
      if (count == 2) return {};
      auto& track = result.tracks[count++];
      const auto tkhd = Child(b, *box, Tag('t', 'k', 'h', 'd'));
      const auto mdia = Child(b, *box, Tag('m', 'd', 'i', 'a'));
      // Edits would change the interpretation of the local CTS transform.
      if (!tkhd || !mdia ||
          ContainsOrMalformed(b, *box, Tag('e', 'd', 't', 's')))
        return {};
      const auto mdhd = Child(b, *mdia, Tag('m', 'd', 'h', 'd'));
      const auto hdlr = Child(b, *mdia, Tag('h', 'd', 'l', 'r'));
      const auto minf = Child(b, *mdia, Tag('m', 'i', 'n', 'f'));
      if (!mdhd || !hdlr || !minf ||
          !Has(*hdlr, hdlr->start + hdlr->header, 12))
        return {};
      const auto handler = b.U32(hdlr->start + hdlr->header + 8);
      if (handler != Tag('v', 'i', 'd', 'e') &&
          handler != Tag('s', 'o', 'u', 'n'))
        return {};
      track.video = handler == Tag('v', 'i', 'd', 'e');
      if (!Duration(b, *tkhd, absolute, true, track.id, track.track_duration) ||
          !Duration(b, *mdhd, absolute, false, track.scale,
                    track.media_duration) ||
          !SelfContained(b, *minf, track.video))
        return {};
    }
    p += box->size;
  }
  if (count != 2 || result.tracks[0].id == result.tracks[1].id ||
      result.tracks[0].video == result.tracks[1].video)
    return {};
  std::array<bool, 2> seen{};
  for (auto p = mvex->start + mvex->header; p < mvex->start + mvex->size;) {
    const auto box = At(b, p, mvex->start + mvex->size);
    if (!box || box->kind != Tag('t', 'r', 'e', 'x') ||
        box->size != box->header + 24)
      return {};
    const auto body = p + box->header;
    if (b.U32(body) != 0u || b.U32(body + 8) != 1u) return {};
    const auto id = b.U32(body + 4);
    const auto index = id == result.tracks[0].id   ? 0u
                       : id == result.tracks[1].id ? 1u
                                                   : 2u;
    if (index == 2 || seen[index]) return {};
    seen[index] = true;
    p += box->size;
  }
  return seen[0] && seen[1] ? std::optional<Initialization>(result)
                            : std::nullopt;
}
struct TrackState {
  std::uint64_t decode_end = 0, samples = 0, presentation_end = 0, last_pts = 0;
};
struct Plan {
  std::uint64_t generation = 0, begin = 0, end = 0;
  std::uint32_t sequence = 0;
  std::array<TrackState, 2> tracks{};
};
struct Run {
  std::size_t body = 0, count = 0;
  std::uint64_t location = 0, bytes = 0;
  std::int64_t min_cts = 0, max_cts = 0;
  bool present = false, unsigned_cts = false;
};
inline std::int64_t Signed(std::uint32_t value) {
  return value <= 0x7fffffffu
             ? static_cast<std::int64_t>(value)
             : -1 - static_cast<std::int64_t>(0xffffffffu - value);
}
inline void Put32(std::uint8_t* bytes, std::uint32_t value) {
  for (std::size_t i = 4; i > 0; --i) {
    bytes[i - 1] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}
enum class FragmentInput { native_signed_cts, stored_cts };
class Admission {
 public:
  // Same FileTransferLayout, Pixel Recorder and native callback queue bounds.
  static constexpr auto max_bytes = AudioAdmission::max_bytes;
  static constexpr auto max_seconds = AudioAdmission::max_seconds;
  static constexpr auto metadata_limit = AudioAdmission::max_metadata_bytes;
  Admission(std::uint64_t budget, std::size_t metadata_budget,
            std::uint64_t audio_start_hns, std::uint64_t video_start_hns,
            FragmentInput input = FragmentInput::native_signed_cts)
      : budget_(budget),
        metadata_budget_(metadata_budget),
        audio_start_(audio_start_hns),
        video_start_(video_start_hns),
        input_(input) {
    if (!budget || budget > max_bytes || !metadata_budget ||
        metadata_budget > metadata_limit ||
        audio_start_hns > max_seconds * 10000000 ||
        video_start_hns > max_seconds * 10000000 ||
        (audio_start_hns && video_start_hns) ||
        (input == FragmentInput::stored_cts &&
         (audio_start_hns || video_start_hns)))
      stop_ = Stop::invalid;
  }
  bool Initialize(Bytes moov, std::uint64_t absolute, std::uint64_t end) {
    if (stop_ != Stop::none || init_ || pending_) return false;
    if (moov.size > metadata_budget_) return Fail(Stop::queue);
    const auto final = Add(absolute, moov.size);
    if (!final || *final != end || end > budget_) return Fail(Stop::invalid);
    init_ = Parse(moov, absolute);
    if (!init_) return Fail(Stop::invalid);
    for (std::size_t i = 0; i != 2; ++i) {
      const auto time = init_->tracks[i].video ? video_start_ : audio_start_;
      const auto value = ScaleCeil(time, 10000000, init_->tracks[i].scale);
      if (!value) return Fail(Stop::invalid);
      // Floor the local start at the native track quantum. The admitted end
      // uses the encoded durations/CTS exactly, never a rounded bitrate/time.
      const auto rest = time % 10000000;
      offsets_[i] =
          *value - ((rest * init_->tracks[i].scale) % 10000000 ? 1 : 0);
    }
    end_ = end;
    return true;
  }
  std::optional<Plan> Reserve(std::uint8_t* mutable_moof, std::size_t size,
                              Bytes mdat, std::uint64_t physical_end) {
    if (stop_ != Stop::none || !init_ || pending_) return {};
    const auto invalid = [&]() -> std::optional<Plan> {
      Fail(Stop::invalid);
      return {};
    };
    if (physical_end != end_ || !mutable_moof) return invalid();
    if (size > metadata_budget_ || mdat.size > metadata_budget_ - size) {
      Fail(Stop::queue);
      return {};
    }
    if (mdat.size != 8 && mdat.size != 16) return invalid();
    const auto compact = mdat.U32(0), kind = mdat.U32(4);
    if (!compact || kind != Tag('m', 'd', 'a', 't')) return invalid();
    std::optional<std::uint64_t> length;
    if (*compact == 1 && mdat.size == 16)
      length = mdat.U64(8);
    else if (*compact > 1 && mdat.size == 8)
      length = *compact;
    if (!length || *length <= mdat.size) return invalid();
    const Bytes b{mutable_moof, size};
    const auto root = At(b, 0, size);
    if (!root || root->kind != Tag('m', 'o', 'o', 'f') || root->size != size)
      return invalid();
    const auto mfhd = Child(b, *root, Tag('m', 'f', 'h', 'd'));
    if (!mfhd || mfhd->size != mfhd->header + 8 ||
        b.U32(mfhd->start + mfhd->header) != 0u)
      return invalid();
    const auto sequence = b.U32(mfhd->start + mfhd->header + 4);
    if (!sequence || sequence_ == 0xffffffffu || *sequence != sequence_ + 1)
      return invalid();
    Plan next{generation_ + 1, end_, 0, *sequence, tracks_};
    std::array<Run, 2> runs{};
    std::array<bool, 2> seen{};
    const auto payload_begin = Add(end_, size + mdat.size);
    if (!payload_begin) return invalid();
    for (auto p = root->header; p < size;) {
      const auto traf = At(b, p, size);
      if (!traf) return invalid();
      if (traf->kind == Tag('t', 'r', 'a', 'f')) {
        const auto tfhd = Child(b, *traf, Tag('t', 'f', 'h', 'd'));
        const auto trun = Child(b, *traf, Tag('t', 'r', 'u', 'n'));
        if (!tfhd || tfhd->size != tfhd->header + 20) return invalid();
        const auto hb = tfhd->start + tfhd->header;
        // MF writes four additional reserved bytes after its absolute base.
        if (b.U32(hb) != 1u || b.U32(hb + 16) != 0u) return invalid();
        std::size_t time_fields = 0;
        for (auto at = traf->start + traf->header;
             at < traf->start + traf->size;) {
          const auto child = At(b, at, traf->start + traf->size);
          if (!child) return invalid();
          if (child->kind == Tag('t', 'f', 'd', 't')) {
            if (++time_fields > 1 || child->size != child->header + 12)
              return invalid();
          } else if (child->kind != Tag('t', 'f', 'h', 'd') &&
                     child->kind != Tag('t', 'r', 'u', 'n'))
            return invalid();
          at += child->size;
        }
        const auto id = b.U32(hb + 4);
        const auto base = b.U64(hb + 8);
        const auto i = id == init_->tracks[0].id   ? 0u
                       : id == init_->tracks[1].id ? 1u
                                                   : 2u;
        if (!base || i == 2 || seen[i]) return invalid();
        seen[i] = true;
        // Native MF keeps a tfhd-only entry for an already ended track. It
        // carries no samples, byte extent or timeline advancement.
        if (!trun) {
          if (traf->size != traf->header + tfhd->size ||
              *base != *payload_begin)
            return invalid();
          p += traf->size;
          continue;
        }
        auto& run = runs[i];
        auto& state = next.tracks[i];
        run.present = true;
        const auto rb = trun->start + trun->header;
        if (!Has(*trun, rb, 12)) return invalid();
        const auto format = b.U32(rb);
        if (format != 0x01000f01u &&
            !(input_ == FragmentInput::stored_cts && format == 0x00000f01u))
          return invalid();
        run.unsigned_cts = format == 0x00000f01u;
        const auto count = b.U32(rb + 4), raw_offset = b.U32(rb + 8);
        if (!count || !*count || !raw_offset ||
            *count != (trun->size - trun->header - 12) / 16 ||
            (trun->size - trun->header - 12) % 16)
          return invalid();
        const auto offset = Signed(*raw_offset);
        if (offset < 0) {
          if (std::uint64_t(-offset) > *base) return invalid();
          run.location = *base - std::uint64_t(-offset);
        } else {
          const auto at = Add(*base, static_cast<std::uint64_t>(offset));
          if (!at) return invalid();
          run.location = *at;
        }
        if (const auto tfdt = Child(b, *traf, Tag('t', 'f', 'd', 't'))) {
          const auto body = tfdt->start + tfdt->header;
          if (!Has(*tfdt, body, 12) || b.U32(body) != 0x01000000u ||
              b.U64(body + 4) != state.decode_end)
            return invalid();
        }
        run.body = rb;
        run.count = *count;
        run.min_cts = std::numeric_limits<std::int64_t>::max();
        run.max_cts = std::numeric_limits<std::int64_t>::min();
        const auto bound = max_seconds * init_->tracks[i].scale;
        for (std::size_t n = 0; n != run.count; ++n) {
          const auto at = rb + 12 + 16 * n;
          const auto duration = b.U32(at), bytes = b.U32(at + 4),
                     composition = b.U32(at + 12);
          if (!duration || !*duration || !bytes || !*bytes || !composition)
            return invalid();
          const auto cts = run.unsigned_cts
                               ? static_cast<std::int64_t>(*composition)
                               : Signed(*composition);
          if (state.decode_end >
                  std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
              offsets_[i] >
                  std::uint64_t(std::numeric_limits<std::int64_t>::max()) -
                      state.decode_end)
            return invalid();
          const auto signed_base =
              static_cast<std::int64_t>(state.decode_end + offsets_[i]);
          if (cts > 0 &&
              signed_base > std::numeric_limits<std::int64_t>::max() - cts)
            return invalid();
          const auto pts = signed_base + cts;
          if (pts < 0 ||
              (state.samples && std::uint64_t(pts) <= state.last_pts))
            return invalid();
          const auto end = Add(static_cast<std::uint64_t>(pts), *duration);
          const auto decode = Add(state.decode_end, *duration),
                     payload = Add(run.bytes, *bytes);
          if (!end || !decode || !payload || state.samples == UINT64_MAX)
            return invalid();
          if (*end > bound || *decode > bound) {
            Fail(Stop::duration);
            return {};
          }
          const auto adjusted = cts + static_cast<std::int64_t>(offsets_[i]);
          run.min_cts = std::min(run.min_cts, adjusted);
          run.max_cts = std::max(run.max_cts, adjusted);
          state.decode_end = *decode;
          state.presentation_end = std::max(state.presentation_end, *end);
          state.last_pts = static_cast<std::uint64_t>(pts);
          ++state.samples;
          run.bytes = *payload;
        }
        if (run.min_cts < INT32_MIN || run.max_cts > UINT32_MAX ||
            (run.min_cts < 0 && run.max_cts > INT32_MAX))
          return invalid();
      } else if (traf->kind != Tag('m', 'f', 'h', 'd'))
        return invalid();
      p += traf->size;
    }
    if (!runs[0].present && !runs[1].present) return invalid();
    const auto begin = Add(end_, size + mdat.size);
    if (!begin) return invalid();
    auto cursor = *begin;
    // Physical track order is unrelated to the handler/track-ID order.
    std::array<std::size_t, 2> order{0, 1};
    if (!runs[0].present ||
        (runs[1].present && runs[1].location < runs[0].location))
      std::swap(order[0], order[1]);
    for (const auto i : order) {
      if (!runs[i].present) continue;
      if (runs[i].location != cursor) return invalid();
      const auto at = Add(cursor, runs[i].bytes);
      if (!at) return invalid();
      cursor = *at;
    }
    if (cursor - *begin != *length - mdat.size) return invalid();
    if (size > budget_ - end_ || *length > budget_ - end_ - size) {
      Fail(Stop::bytes);
      return {};
    }
    const auto final = Add(end_, size + *length);
    if (!final || *final != cursor || generation_ == UINT64_MAX)
      return invalid();
    next.end = *final;
    // No mutation occurs until every track, byte extent, clock and quota
    // passes.
    for (std::size_t i = 0; i != 2; ++i) {
      const auto& run = runs[i];
      if (!run.present) continue;
      mutable_moof[run.body] = run.min_cts < 0 ? 1 : 0;
      for (std::size_t n = 0; n != run.count; ++n) {
        const auto at = run.body + 12 + n * 16 + 12;
        const auto raw = *b.U32(at);
        const auto value =
            (run.unsigned_cts ? static_cast<std::int64_t>(raw) : Signed(raw)) +
            static_cast<std::int64_t>(offsets_[i]);
        Put32(mutable_moof + at, static_cast<std::uint32_t>(value));
      }
    }
    pending_ = next;
    ++generation_;
    return pending_;
  }
  bool Commit(const Plan& plan, std::uint64_t physical_end) {
    if (stop_ != Stop::none || !pending_ || !Same(plan, *pending_) ||
        physical_end != plan.end)
      return Fail(Stop::invalid);
    end_ = plan.end;
    sequence_ = plan.sequence;
    tracks_ = plan.tracks;
    pending_.reset();
    ++fragments_;
    return true;
  }
  std::optional<std::array<DurationPatch, 5>> ClosedPatches(
      bool native_closed, bool quota_witness,
      std::uint32_t closed_movie_scale) const {
    if (!native_closed || !init_ || pending_ || !fragments_ ||
        tracks_[0].samples < 2 || tracks_[1].samples < 2 ||
        (stop_ != Stop::none &&
         !(quota_witness && (stop_ == Stop::bytes || stop_ == Stop::duration ||
                             stop_ == Stop::queue))))
      return {};
    if (closed_movie_scale != init_->tracks[0].scale &&
        closed_movie_scale != init_->tracks[1].scale)
      return {};
    std::array<DurationPatch, 5> result{};
    std::uint64_t movie = 0;
    for (std::size_t i = 0; i != 2; ++i) {
      const auto value = ScaleCeil(tracks_[i].presentation_end,
                                   init_->tracks[i].scale, closed_movie_scale);
      if (!value) return {};
      movie = std::max(movie, *value);
      // mdhd is the coded decode duration; tkhd is the presentation end in the
      // movie clock. No historical archive is rewritten by this owner.
      result[1 + 2 * i] = {init_->tracks[i].track_duration.offset, *value,
                           init_->tracks[i].track_duration.width};
      result[2 + 2 * i] = {init_->tracks[i].media_duration.offset,
                           tracks_[i].decode_end,
                           init_->tracks[i].media_duration.width};
    }
    result[0] = {init_->movie_duration.offset, movie,
                 init_->movie_duration.width};
    for (const auto& patch : result)
      if (patch.width == 4 && patch.value > UINT32_MAX) return {};
    return result;
  }
  Stop stop() const { return stop_; }
  std::uint64_t end() const { return end_; }
  const std::array<TrackState, 2>& tracks() const { return tracks_; }
  const std::optional<Initialization>& initialization() const { return init_; }
  bool Trailer(std::uint64_t begin, std::uint64_t bytes) {
    if (stop_ != Stop::none || pending_ || !init_ || begin != end_)
      return Fail(Stop::invalid);
    if (bytes > budget_ - end_) return Fail(Stop::bytes);
    return true;
  }
  void ExternalLimit(Stop reason) {
    if (!pending_ && (reason == Stop::bytes || reason == Stop::duration ||
                      reason == Stop::queue))
      Fail(reason);
    else
      Fail(Stop::invalid);
  }
  void IoFailed() { stop_ = Stop::io; }
  void Retire() { stop_ = Stop::retired; }

 private:
  bool Fail(Stop reason) {
    if (stop_ == Stop::none) stop_ = reason;
    return false;
  }
  static bool Same(const Plan& a, const Plan& b) {
    if (a.generation != b.generation || a.begin != b.begin || a.end != b.end ||
        a.sequence != b.sequence)
      return false;
    for (std::size_t i = 0; i != 2; ++i) {
      if (a.tracks[i].decode_end != b.tracks[i].decode_end ||
          a.tracks[i].samples != b.tracks[i].samples ||
          a.tracks[i].presentation_end != b.tracks[i].presentation_end ||
          a.tracks[i].last_pts != b.tracks[i].last_pts)
        return false;
    }
    return true;
  }
  const std::uint64_t budget_;
  const std::size_t metadata_budget_;
  const std::uint64_t audio_start_, video_start_;
  const FragmentInput input_;
  Stop stop_ = Stop::none;
  std::optional<Initialization> init_;
  std::optional<Plan> pending_;
  std::array<TrackState, 2> tracks_{};
  std::array<std::uint64_t, 2> offsets_{};
  std::uint64_t end_ = 0, generation_ = 0, fragments_ = 0;
  std::uint32_t sequence_ = 0;
};
}  // namespace nonet::fmp4::av
