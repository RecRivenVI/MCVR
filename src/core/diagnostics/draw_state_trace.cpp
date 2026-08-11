#include "core/diagnostics/draw_state_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

namespace mcvr::diagnostics {
namespace {

enum class RecordKind {
    Meta,
    Frame,
    Uniform,
    Descriptor,
    Lifecycle,
    Texture,
    Draw,
};

struct TraceWriter {
    std::once_flag initializeOnce;
    std::mutex mutex;
    std::ofstream file;
    std::ofstream journal;
    uint64_t sequence = 0;
    uint64_t journalBytes = 0;
    std::chrono::steady_clock::time_point start{};
    std::string path;
    bool enabled = false;
    bool rolling = false;
    uint64_t segment = 0;
    uint64_t maxEvents = 32768;
    uint64_t flushEvery = 16;
    uint64_t eventCount = 0;
    uint64_t pendingFlush = 0;

    void initialize() noexcept {
        try {
            const char *rawPath = std::getenv("MCVR_DRAW_STATE_TRACE_PATH");
            if (rawPath == nullptr || *rawPath == '\0') return;

            path = rawPath;
            const char *rawRolling = std::getenv("MCVR_DRAW_STATE_TRACE_ROLLING");
            rolling = rawRolling != nullptr && std::string_view(rawRolling) == "1";
            if (const char *rawMax = std::getenv("MCVR_DRAW_STATE_TRACE_MAX_EVENTS")) {
                char *end = nullptr;
                const unsigned long long parsed = std::strtoull(rawMax, &end, 10);
                if (end != rawMax && *end == '\0') {
                    maxEvents = std::clamp<uint64_t>(parsed, 256, 100000);
                }
            }
            if (const char *rawFlush = std::getenv("MCVR_DRAW_STATE_TRACE_FLUSH_EVERY")) {
                char *end = nullptr;
                const unsigned long long parsed = std::strtoull(rawFlush, &end, 10);
                if (end != rawFlush && *end == '\0') {
                    flushEvery = std::clamp<uint64_t>(parsed, 1, 1024);
                }
            }

            file.open(path, std::ios::out | std::ios::app | std::ios::binary);
            if (!file.is_open()) return;
            if (const char *journalPath = std::getenv("MCVR_DRAW_STATE_LIFECYCLE_PATH")) {
                if (*journalPath != '\0') {
                    journal.open(journalPath, std::ios::out | std::ios::app | std::ios::binary);
                    journal << "# lifecycle version=1 byteLimit=268435456\n";
                    journal.flush();
                }
            }
            start = std::chrono::steady_clock::now();
            enabled = true;
            file << "# MCVR_DRAW_STATE_TRACE version=1 maxEvents=" << maxEvents
                 << " flushEvery=" << flushEvery << " rolling=" << rolling
                 << " segment=0" << '\n';
            file.flush();
        } catch (...) {
            enabled = false;
        }
    }

    bool isEnabled() noexcept {
        std::call_once(initializeOnce, [this] { initialize(); });
        return enabled;
    }

    static const char *kindName(RecordKind kind) noexcept {
        switch (kind) {
            case RecordKind::Meta: return "META";
            case RecordKind::Frame: return "FRAME";
            case RecordKind::Uniform: return "UNIFORM";
            case RecordKind::Descriptor: return "DESCRIPTOR";
            case RecordKind::Lifecycle: return "LIFECYCLE";
            case RecordKind::Texture: return "TEXTURE";
            case RecordKind::Draw: return "DRAW";
        }
        return "UNKNOWN";
    }

    static std::string sanitize(std::string_view value) {
        std::string result;
        result.reserve(value.size());
        for (char c : value) {
            result.push_back(c == '\t' || c == '\r' || c == '\n' ? '_' : c);
        }
        return result;
    }

    static std::string hex(uint64_t value) {
        std::ostringstream stream;
        stream << "0x" << std::hex << value << std::dec;
        return stream.str();
    }

    static uint64_t threadToken() noexcept {
        return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    bool write(RecordKind kind, std::string_view fields) noexcept {
        try {
            if (!isEnabled()) return false;
            std::lock_guard lock(mutex);
            if (!enabled || !file.is_open()) return false;
            if (eventCount >= maxEvents) {
                if (!rolling) return false;
                file.flush();
                file.close();
                ++segment;
                // Two bounded segments retain the recent tail through a native crash.
                file.open(segment % 2 == 0 ? path : path + ".1",
                          std::ios::out | std::ios::trunc | std::ios::binary);
                if (!file.is_open()) { enabled = false; return false; }
                file << "# MCVR_DRAW_STATE_TRACE version=1 segment=" << segment
                     << " maxEvents=" << maxEvents << " rolling=1\n";
                file.flush();
                eventCount = 0;
                pendingFlush = 0;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            ++sequence;
            file << elapsed << '\t' << threadToken() << '\t' << kindName(kind) << '\t'
                 << "seq=" << sequence << '\t' << fields << '\n';
            // Keep definitions across rolling segments without retaining GPU resources.
            const bool archive = kind == RecordKind::Descriptor || kind == RecordKind::Lifecycle ||
                kind == RecordKind::Texture || (kind == RecordKind::Frame &&
                    !fields.starts_with("event=DRAW_RETURN\t"));
            if (archive && journal.is_open()) {
                std::ostringstream line;
                line << elapsed << '\t' << threadToken() << '\t' << kindName(kind) << '\t'
                     << "seq=" << sequence << '\t' << fields << '\n';
                const auto entry = line.str();
                if (journalBytes + entry.size() > 268435456) {
                    journal << "# INCOMPLETE byte limit reached seq=" << sequence << '\n';
                    journal.flush();
                    journal.close();
                } else {
                    journal << entry;
                    journal.flush();
                    journalBytes += entry.size();
                }
            }
            ++eventCount;
            if (++pendingFlush >= flushEvery) {
                file.flush();
                pendingFlush = 0;
            }
            if (!file.good()) enabled = false;
            return true;
        } catch (...) {
            enabled = false;
            return false;
        }
    }
};

TraceWriter &writer() noexcept {
    static TraceWriter instance;
    return instance;
}

void appendHandle(std::ostringstream &stream, std::string_view name, uint64_t value) {
    stream << name << '=' << TraceWriter::hex(value) << '\t';
}

void appendString(std::ostringstream &stream, std::string_view name, std::string_view value) {
    stream << name << '=' << TraceWriter::sanitize(value) << '\t';
}

void appendBytes(std::ostringstream &stream, const uint8_t *data, size_t size) {
    if (data == nullptr || size == 0) {
        stream << "preview=\t";
        return;
    }
    stream << "preview=" << std::hex << std::setfill('0');
    const size_t previewSize = std::min<size_t>(size, 256);
    for (size_t i = 0; i < previewSize; ++i) stream << std::setw(2) << static_cast<unsigned>(data[i]);
    stream << std::dec << '\t';
}

} // namespace

bool drawStateTraceEnabled() noexcept {
    return writer().isEnabled();
}

void recordDescriptorLifecycle(std::string_view event, uint64_t table,
                               uint64_t layout, uint64_t pool) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    appendString(stream, "event", event);
    appendHandle(stream, "table", table);
    appendHandle(stream, "pipelineLayout", layout);
    appendHandle(stream, "pool", pool);
    trace.write(RecordKind::Lifecycle, stream.str());
}

void recordFrame(std::string_view event,
                 uint32_t frameIndex,
                 bool frameSubmitted,
                 uint64_t fence,
                 uint64_t overlayCommandBuffer) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    appendString(stream, "event", event);
    stream << "frame=" << frameIndex << '\t'
           << "submitted=" << (frameSubmitted ? 1 : 0) << '\t';
    appendHandle(stream, "fence", fence);
    appendHandle(stream, "overlayCmd", overlayCommandBuffer);
    trace.write(RecordKind::Frame, stream.str());
}

void recordUniform(std::string_view source,
                   uint32_t frameIndex,
                   bool frameSubmitted,
                   uint32_t shaderId,
                   uint32_t vertexId,
                   uint32_t indexId,
                   int32_t patchIndexId,
                   uint32_t uniformOffset,
                   const uint8_t *data,
                   size_t size) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    appendString(stream, "source", source);
    stream << "frame=" << frameIndex << '\t'
           << "submitted=" << (frameSubmitted ? 1 : 0) << '\t'
           << "shader=" << shaderId << '\t'
           << "vertexId=" << vertexId << '\t'
           << "indexId=" << indexId << '\t'
           << "patchId=" << patchIndexId << '\t'
           << "offset=" << uniformOffset << '\t'
           << "size=" << size << '\t';
    appendBytes(stream, data, size);
    trace.write(RecordKind::Uniform, stream.str());
}

void recordDescriptorImage(uint32_t frameIndex,
                           uint64_t descriptorTableObject,
                           uint64_t pipelineLayout,
                           uint64_t descriptorSet,
                           int32_t set,
                           int32_t binding,
                           int32_t index,
                           uint64_t samplerObject,
                           uint64_t sampler,
                           uint64_t imageObject,
                           uint64_t image,
                           uint64_t imageView,
                           uint32_t imageLayout) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    stream << "frame=" << frameIndex << '\t'
           << "set=" << set << '\t'
           << "binding=" << binding << '\t'
           << "index=" << index << '\t'
           << "layout=" << imageLayout << '\t';
    appendHandle(stream, "table", descriptorTableObject);
    appendHandle(stream, "pipelineLayout", pipelineLayout);
    appendHandle(stream, "descriptorSet", descriptorSet);
    appendHandle(stream, "samplerObject", samplerObject);
    appendHandle(stream, "sampler", sampler);
    appendHandle(stream, "imageObject", imageObject);
    appendHandle(stream, "image", image);
    appendHandle(stream, "imageView", imageView);
    trace.write(RecordKind::Descriptor, stream.str());
}

void recordDescriptorBuffer(uint32_t frameIndex,
                            uint64_t descriptorTableObject,
                            uint64_t pipelineLayout,
                            uint64_t descriptorSet,
                            int32_t set,
                            int32_t binding,
                            uint64_t bufferObject,
                            uint64_t buffer,
                            uint64_t descriptorOffset,
                            uint64_t descriptorRange,
                            uint64_t bufferSize) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    stream << "frame=" << frameIndex << '\t'
           << "set=" << set << '\t'
           << "binding=" << binding << '\t'
           << "offset=" << descriptorOffset << '\t'
           << "range=" << descriptorRange << '\t'
           << "bufferSize=" << bufferSize << '\t';
    appendHandle(stream, "table", descriptorTableObject);
    appendHandle(stream, "pipelineLayout", pipelineLayout);
    appendHandle(stream, "descriptorSet", descriptorSet);
    appendHandle(stream, "bufferObject", bufferObject);
    appendHandle(stream, "buffer", buffer);
    trace.write(RecordKind::Descriptor, stream.str());
}

void recordTexture(std::string_view event,
                   uint32_t textureId,
                   uint32_t fallbackId,
                   uint32_t frameIndex,
                   bool frameSubmitted,
                   uint64_t imageObject,
                   uint64_t image,
                   uint64_t samplerObject,
                   uint64_t sampler,
                   uint32_t width,
                   uint32_t height,
                   uint32_t mipLevels,
                   uint32_t format,
                   uint64_t queuedUploadBytes) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    appendString(stream, "event", event);
    stream << "textureId=" << textureId << '\t'
           << "fallbackId=" << fallbackId << '\t'
           << "frame=" << frameIndex << '\t'
           << "submitted=" << (frameSubmitted ? 1 : 0) << '\t'
           << "width=" << width << '\t'
           << "height=" << height << '\t'
           << "mipLevels=" << mipLevels << '\t'
           << "format=" << format << '\t'
           << "queuedUploadBytes=" << queuedUploadBytes << '\t';
    appendHandle(stream, "imageObject", imageObject);
    appendHandle(stream, "image", image);
    appendHandle(stream, "samplerObject", samplerObject);
    appendHandle(stream, "sampler", sampler);
    trace.write(RecordKind::Texture, stream.str());
}

void recordDraw(std::string_view source,
                uint32_t frameIndex,
                bool frameSubmitted,
                uint32_t overlayMode,
                uint32_t shaderId,
                std::string_view shaderKey,
                uint64_t commandBuffer,
                uint64_t pipeline,
                uint64_t pipelineLayout,
                uint64_t descriptorTableObject,
                std::span<const uint64_t> descriptorSets,
                uint64_t vertexObject,
                uint64_t vertexBuffer,
                uint64_t vertexBufferSize,
                uint64_t indexObject,
                uint64_t indexBuffer,
                uint64_t indexBufferSize,
                uint64_t patchIndexObject,
                uint64_t patchIndexBuffer,
                uint64_t patchIndexBufferSize,
                uint32_t uniformOffset,
                uint32_t indexCount,
                uint32_t patchIndexCount,
                uint32_t instanceCount,
                uint32_t indexType) noexcept {
    auto &trace = writer();
    if (!trace.isEnabled()) return;
    std::ostringstream stream;
    appendString(stream, "source", source);
    appendString(stream, "shaderKey", shaderKey);
    stream << "frame=" << frameIndex << '\t'
           << "submitted=" << (frameSubmitted ? 1 : 0) << '\t'
           << "overlayMode=" << overlayMode << '\t'
           << "shader=" << shaderId << '\t'
           << "uniformOffset=" << uniformOffset << '\t'
           << "indexCount=" << indexCount << '\t'
           << "patchIndexCount=" << patchIndexCount << '\t'
           << "instanceCount=" << instanceCount << '\t'
           << "indexType=" << indexType << '\t';
    appendHandle(stream, "commandBuffer", commandBuffer);
    appendHandle(stream, "pipeline", pipeline);
    appendHandle(stream, "pipelineLayout", pipelineLayout);
    appendHandle(stream, "descriptorTable", descriptorTableObject);
    stream << "descriptorSets=";
    for (size_t i = 0; i < descriptorSets.size(); ++i) {
        if (i != 0) stream << ',';
        stream << TraceWriter::hex(descriptorSets[i]);
    }
    stream << '\t';
    appendHandle(stream, "vertexObject", vertexObject);
    appendHandle(stream, "vertexBuffer", vertexBuffer);
    stream << "vertexSize=" << vertexBufferSize << '\t';
    appendHandle(stream, "indexObject", indexObject);
    appendHandle(stream, "indexBuffer", indexBuffer);
    stream << "indexSize=" << indexBufferSize << '\t';
    appendHandle(stream, "patchObject", patchIndexObject);
    appendHandle(stream, "patchBuffer", patchIndexBuffer);
    stream << "patchSize=" << patchIndexBufferSize << '\t';
    trace.write(RecordKind::Draw, stream.str());
}

} // namespace mcvr::diagnostics
