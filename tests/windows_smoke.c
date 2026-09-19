/* Exercise the delivered DLL through its Chromium import library, in Release. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chromium/mtrr_blob.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); \
} } while (0)

static const uint8_t wav[] = {
    'R','I','F','F', 44,0,0,0, 'W','A','V','E',
    'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
    0x80,0xbb,0,0, 0,0x77,1,0, 2,0, 16,0,
    'd','a','t','a', 8,0,0,0, 0,0, 0xff,0x7f, 0,0x80, 0x34,0x12
};

static int read_wav(void *opaque, uint8_t *buffer, int size)
{
    int *position = opaque;
    int remaining = (int)sizeof(wav) - *position;
    if (!remaining)
        return AVERROR_EOF;
    if (size > remaining)
        size = remaining;
    memcpy(buffer, wav + *position, size);
    *position += size;
    return size;
}

static int64_t seek_wav(void *opaque, int64_t offset, int whence)
{
    int *position = opaque;
    int64_t next;
    if (whence == AVSEEK_SIZE)
        return sizeof(wav);
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET)
        next = offset;
    else if (whence == SEEK_CUR)
        next = *position + offset;
    else if (whence == SEEK_END)
        next = sizeof(wav) + offset;
    else
        return AVERROR(EINVAL);
    if (next < 0 || next > sizeof(wav))
        return AVERROR(EINVAL);
    *position = (int)next;
    return next;
}

static void decode_wav(void)
{
    int position = 0;
    uint8_t *buffer = av_malloc(4096);
    AVIOContext *io;
    AVFormatContext *format = avformat_alloc_context();
    AVCodecContext *decoder;
    AVPacket packet;
    AVFrame *frame = av_frame_alloc();
    static const int16_t expected[] = {0, 32767, -32768, 0x1234};
    CHECK(buffer && format && frame);
    io = avio_alloc_context(buffer, 4096, 0, &position, read_wav, NULL, seek_wav);
    CHECK(io);
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    CHECK(avformat_open_input(&format, NULL, NULL, NULL) == 0);
    CHECK(avformat_find_stream_info(format, NULL) >= 0);
    CHECK(format->nb_streams == 1);
    CHECK(format->streams[0]->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE);
    decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE));
    CHECK(decoder);
    CHECK(avcodec_parameters_to_context(decoder, format->streams[0]->codecpar) == 0);
    CHECK(avcodec_open2(decoder, decoder->codec, NULL) == 0);
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;
    CHECK(av_read_frame(format, &packet) == 0);
    CHECK(avcodec_send_packet(decoder, &packet) == 0);
    CHECK(avcodec_receive_frame(decoder, frame) == 0);
    CHECK(frame->nb_samples == 4 && frame->format == AV_SAMPLE_FMT_S16);
    CHECK(memcmp(frame->data[0], expected, sizeof(expected)) == 0);
    av_packet_unref(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    av_free(io->buffer);
    av_free(io);
}

static void check_mtrr_slot(const char *path)
{
    HANDLE file;
    HANDLE mapping;
    const uint8_t *base;
    const uint8_t *hit = NULL;
    DWORD size;
    DWORD i;
    unsigned hits = 0;
    uint32_t ver = 0, pklen = 0, magic = 0, cbkey = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    size = GetFileSize(file, NULL);
    CHECK(size >= kMtrrSlotSize);
    mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK(mapping);
    base = (const uint8_t *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CHECK(base);
    for (i = 0; i + kMtrrSlotSize <= size; i++) {
        if (memcmp(base + i, kMtrrSlotCookieBegin, 16) == 0) {
            hits++;
            hit = base + i;
        }
    }
    CHECK(hits == 1);
    CHECK(hit);
    CHECK(memcmp(hit + kMtrrSlotCookieEndOff, kMtrrSlotCookieEnd, 16) == 0);
    memcpy(&ver, hit + kMtrrSlotVersionOff, 4);
    memcpy(&pklen, hit + kMtrrSlotPubkeyLenOff, 4);
    memcpy(&magic, hit + kMtrrSlotPubkeyOff, 4);
    memcpy(&cbkey, hit + kMtrrSlotPubkeyOff + 4, 4);
    CHECK(ver == kMtrrSlotVersion);
    CHECK(pklen == kMtrrEccPubLen);
    CHECK(magic == 0x314B4345u);
    CHECK(cbkey == kMtrrEccCoordLen);
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
}

static void decode_opus(void)
{
    /* A standard 20 ms Opus silence packet; tests the statically linked dependency. */
    static const uint8_t silence[] = {0xf8, 0xff, 0xfe};
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    AVCodecContext *decoder;
    AVFrame *frame = av_frame_alloc();
    AVPacket packet;
    CHECK(codec && frame);
    decoder = avcodec_alloc_context3(codec);
    CHECK(decoder);
    decoder->sample_rate = 48000;
    decoder->channels = 2;
    decoder->channel_layout = AV_CH_LAYOUT_STEREO;
    CHECK(avcodec_open2(decoder, codec, NULL) == 0);
    av_init_packet(&packet);
    CHECK(av_new_packet(&packet, sizeof(silence)) == 0);
    memcpy(packet.data, silence, sizeof(silence));
    CHECK(avcodec_send_packet(decoder, &packet) == 0);
    CHECK(avcodec_receive_frame(decoder, frame) == 0);
    CHECK(frame->nb_samples == 960 && frame->channels == 2);
    av_packet_unref(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
}

int main(int argc, char **argv)
{
    HMODULE module;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_EXPORT_DIRECTORY *export_directory;
    CHECK(argc == 2);
    module = LoadLibraryA(argv[1]);
    CHECK(module);
    dos = (const IMAGE_DOS_HEADER *)module;
    nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)module + dos->e_lfanew);
#ifdef EXPECT_HISTORICAL_PE
    CHECK(nt->OptionalHeader.MajorLinkerVersion == 14 && nt->OptionalHeader.MinorLinkerVersion == 0);
    CHECK(nt->OptionalHeader.MajorOperatingSystemVersion == 5 && nt->OptionalHeader.MinorOperatingSystemVersion == 1);
    CHECK(nt->OptionalHeader.MajorSubsystemVersion == 5 && nt->OptionalHeader.MinorSubsystemVersion == 1);
    CHECK(nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI);
    CHECK(nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE);
    CHECK(nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF);
    {
        const IMAGE_DATA_DIRECTORY *entry = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        const IMAGE_DEBUG_DIRECTORY *debug = (const IMAGE_DEBUG_DIRECTORY *)((const uint8_t *)module + entry->VirtualAddress);
        unsigned i;
        int found = 0;
        for (i = 0; i < entry->Size / sizeof(*debug); ++i) {
            if (debug[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW) {
                const char *cv = (const char *)module + debug[i].AddressOfRawData;
                CHECK(debug[i].SizeOfData >= 24 + sizeof("ffmpeg.dll.pdb"));
                CHECK(memcmp(cv, "RSDS", 4) == 0);
                CHECK(strcmp(cv + 24, "ffmpeg.dll.pdb") == 0);
                found = 1;
            }
        }
        CHECK(found);
    }
#endif
    export_directory = (const IMAGE_EXPORT_DIRECTORY *)((const uint8_t *)module +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
#include "export_checks.h"
    check_mtrr_slot(argv[1]);
    CHECK(GetProcAddress(module, "av_log") == NULL);
    CHECK((avcodec_find_decoder(AV_CODEC_ID_H264) != NULL) == EXPECT_H264);
    CHECK((avcodec_find_decoder(AV_CODEC_ID_AAC) != NULL) == EXPECT_H264);
    decode_wav();
    decode_opus();
    CHECK(FreeLibrary(module));
    puts("Chromium exports, no logging, WAV demux/PCM decode and Opus decode passed.");
    return 0;
}
