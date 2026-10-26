/* OverPass Wine: xWMA sounds through XAudio2 (FAudio, winegstreamer's WMA decoder and GStreamer's avdec_wma*).
 * Plays an xWMA file through an XAudio2 source voice, the way games do, then decodes it through the same WMA
 * decoder transform FAudio uses and checks that the result isn't silence. Exits 0 when both work. Without a
 * decoder FAudio still plays the voice, as silence, so only the second check shows whether games get sound.
 *   x86_64-w64-mingw32-gcc -O1 xwma.c -o xwma.exe -lxaudio2_9 -lmfplat -lmfuuid -lole32
 *   xwma.exe C:\path\to\sound.xwma */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <xaudio2.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <math.h>
#include <stdio.h>

DEFINE_GUID(CLSID_WMADecoder, 0x2eeb4adf, 0x4578, 0x4d10, 0xbc, 0xa7, 0xbb, 0x95, 0x5f, 0x56, 0x32, 0x0a);

struct xwma
{
    BYTE *file;
    WAVEFORMATEX *format;
    UINT32 *packets, packet_count;
    BYTE *data;
    DWORD data_size;
};

static BOOL load(const char *path, struct xwma *x)
{
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    x->file = malloc(size);
    fread(x->file, 1, size, f);
    fclose(f);
    if (size < 12 || memcmp(x->file, "RIFF", 4) || memcmp(x->file + 8, "XWMA", 4)) return FALSE;

    for (BYTE *chunk = x->file + 12; chunk + 8 <= x->file + size; chunk += 8 + ((*(DWORD *)(chunk + 4) + 1) & ~1))
    {
        DWORD length = *(DWORD *)(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4)) x->format = (WAVEFORMATEX *)(chunk + 8);
        else if (!memcmp(chunk, "dpds", 4)) x->packets = (UINT32 *)(chunk + 8), x->packet_count = length / 4;
        else if (!memcmp(chunk, "data", 4)) x->data = chunk + 8, x->data_size = length;
    }
    return x->format && x->packets && x->data;
}

static int play(const struct xwma *x)
{
    IXAudio2 *xaudio;
    IXAudio2MasteringVoice *master;
    IXAudio2SourceVoice *voice;
    HRESULT hr;

    if (FAILED(hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(hr = IXAudio2_CreateMasteringVoice(xaudio, &master, XAUDIO2_DEFAULT_CHANNELS,
                                                  XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects)))
    {
        printf("FAIL XAudio2: no engine or output device (%#lx)\n", hr);
        return 1;
    }
    if (FAILED(hr = IXAudio2_CreateSourceVoice(xaudio, &voice, x->format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                               NULL, NULL, NULL)))
    {
        printf("FAIL XAudio2: no xWMA source voice (%#lx)\n", hr);
        return 1;
    }

    XAUDIO2_BUFFER buffer = {XAUDIO2_END_OF_STREAM, x->data_size, x->data};
    XAUDIO2_BUFFER_WMA wma = {x->packets, x->packet_count};
    if (FAILED(hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buffer, &wma)) ||
        FAILED(hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW)))
    {
        printf("FAIL XAudio2: can't play the buffer (%#lx)\n", hr);
        return 1;
    }
    XAUDIO2_VOICE_STATE state = {0};
    for (int i = 0; i < 20 && state.SamplesPlayed < x->format->nSamplesPerSec / 2; i++)
    {
        Sleep(100);
        IXAudio2SourceVoice_GetState(voice, &state, 0);
    }
    IXAudio2_Release(xaudio);
    if (state.SamplesPlayed < x->format->nSamplesPerSec / 2)
    {
        printf("FAIL XAudio2: only %llu samples played in 2 s\n", state.SamplesPlayed);
        return 1;
    }
    printf("PASS XAudio2: %llu samples played (silent if the decoder check fails)\n", state.SamplesPlayed);
    return 0;
}

/* FAudio's settings for WMA2 xWMA: no codec data in the file, so the flags xWMAEncode always uses. */
static int decode(const struct xwma *x)
{
    static const BYTE codec_data[16] = {0, 0, 0, 0, 31};
    IMFTransform *decoder;
    IMFMediaType *type;
    HRESULT hr;

    if (x->format->wFormatTag != WAVE_FORMAT_WMAUDIO2)
    {
        printf("     (decoder check skipped: format %#x, only WMA2 is checked)\n", x->format->wFormatTag);
        return 0;
    }
    if (FAILED(hr = CoCreateInstance(&CLSID_WMADecoder, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform,
                                     (void **)&decoder)))
    {
        printf("FAIL decoder: no WMA decoder (%#lx)\n", hr);
        return 1;
    }
    MFCreateMediaType(&type);
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_WMAudioV8);
    IMFMediaType_SetBlob(type, &MF_MT_USER_DATA, codec_data, sizeof(codec_data));
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, x->format->nBlockAlign);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, x->format->wBitsPerSample);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, x->format->nAvgBytesPerSec);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, x->format->nChannels);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, x->format->nSamplesPerSec);
    hr = IMFTransform_SetInputType(decoder, 0, type, 0);
    IMFMediaType_Release(type);
    if (FAILED(hr))
    {
        printf("FAIL decoder: WMA2 input refused (%#lx)\n", hr);
        return 1;
    }
    if (FAILED(hr = IMFTransform_GetOutputAvailableType(decoder, 0, 0, &type)) ||
        FAILED(hr = IMFTransform_SetOutputType(decoder, 0, type, 0)))
    {
        printf("FAIL decoder: no output type (%#lx)\n", hr);
        return 1;
    }
    GUID subtype;
    UINT32 bits = 0;
    IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    IMFMediaType_Release(type);
    BOOL is_float = IsEqualGUID(&subtype, &MFAudioFormat_Float);
    MFT_OUTPUT_STREAM_INFO info;
    IMFTransform_GetOutputStreamInfo(decoder, 0, &info);

    double peak = 0;
    DWORD decoded = 0;
    for (DWORD offset = 0; offset + x->format->nBlockAlign <= x->data_size; offset += x->format->nBlockAlign)
    {
        IMFMediaBuffer *buffer;
        IMFSample *sample;
        BYTE *p;
        MFCreateMemoryBuffer(x->format->nBlockAlign, &buffer);
        IMFMediaBuffer_Lock(buffer, &p, NULL, NULL);
        memcpy(p, x->data + offset, x->format->nBlockAlign);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, x->format->nBlockAlign);
        MFCreateSample(&sample);
        IMFSample_AddBuffer(sample, buffer);
        IMFMediaBuffer_Release(buffer);
        hr = IMFTransform_ProcessInput(decoder, 0, sample, 0);
        IMFSample_Release(sample);
        if (FAILED(hr))
        {
            printf("FAIL decoder: input refused at byte %lu (%#lx)\n", offset, hr);
            return 1;
        }

        for (;;)
        {
            MFT_OUTPUT_DATA_BUFFER output = {0};
            DWORD status, length;
            MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 65536, &buffer);
            MFCreateSample(&output.pSample);
            IMFSample_AddBuffer(output.pSample, buffer);
            hr = IMFTransform_ProcessOutput(decoder, 0, 1, &output, &status);
            if (SUCCEEDED(hr))
            {
                IMFMediaBuffer_Lock(buffer, &p, NULL, &length);
                for (DWORD i = 0; i < length; i += bits / 8)
                {
                    double v = is_float ? fabs(*(float *)(p + i)) : abs(*(short *)(p + i)) / 32768.0;
                    if (v > peak) peak = v;
                }
                decoded += length;
                IMFMediaBuffer_Unlock(buffer);
            }
            IMFMediaBuffer_Release(buffer);
            IMFSample_Release(output.pSample);
            if (output.pEvents) IMFCollection_Release(output.pEvents);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(hr))
            {
                printf("FAIL decoder: output error (%#lx)\n", hr);
                return 1;
            }
        }
    }
    IMFTransform_Release(decoder);

    if (!decoded || peak < 0.01)
    {
        printf("FAIL decoder: %lu bytes, peak %.4f (silence)\n", decoded, peak);
        return 1;
    }
    printf("PASS decoder: %lu bytes of %s, peak %.3f\n", decoded, is_float ? "float" : "PCM", peak);
    return 0;
}

int main(int argc, char **argv)
{
    struct xwma x = {0};
    if (argc < 2)
    {
        printf("usage: xwma.exe <xWMA file>\n");
        return 2;
    }
    if (!load(argv[1], &x))
    {
        printf("FAIL %s: not an xWMA file\n", argv[1]);
        return 1;
    }
    printf("     format %#x, %u channels, %lu Hz, %u-byte blocks, %u packets\n", x.format->wFormatTag,
           x.format->nChannels, x.format->nSamplesPerSec, x.format->nBlockAlign, x.packet_count);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    int failures = play(&x) + decode(&x);
    MFShutdown();
    return failures;
}
