#pragma once

#include <QByteArray>
#include <zlib.h>

inline bool decodeZipPayload(const QByteArray &payload, quint16 method,
                             quint32 size, QByteArray &data)
{
    if (method == 0) {
        data = payload;
        return data.size() == size;
    }
    if (method != 8 || !size) return false;
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
    data.resize(size);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.constData()));
    stream.avail_in = static_cast<uInt>(payload.size());
    stream.next_out = reinterpret_cast<Bytef *>(data.data());
    stream.avail_out = size;
    const bool ok = inflate(&stream, Z_FINISH) == Z_STREAM_END &&
                    stream.total_out == size && !stream.avail_in;
    inflateEnd(&stream);
    return ok;
}
