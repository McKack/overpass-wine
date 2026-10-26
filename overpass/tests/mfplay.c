/* OverPass Wine: game video through Media Foundation (winegstreamer and GStreamer). Opens a file with a source
 * reader, the way games' video players do, asks for decoded video (NV12) and audio (PCM), and reads the first
 * samples. Exits 0 when every video and audio stream the file has decodes.
 *   x86_64-w64-mingw32-gcc -O1 mfplay.c -o mfplay.exe -lmfplat -lmfreadwrite -lmfuuid -lole32
 *   mfplay.exe C:\path\to\video.mp4 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>

static int decode(IMFSourceReader *reader, DWORD stream, const GUID *major, const GUID *subtype, const char *what)
{
    IMFMediaType *type;
    MFCreateMediaType(&type);
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, major);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, subtype);
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, stream, NULL, type);
    IMFMediaType_Release(type);
    if (FAILED(hr))
    {
        printf("FAIL %s: no decoder for it (%#lx)\n", what, hr);
        return 1;
    }

    UINT64 size = 0;
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader, stream, &type)))
    {
        IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
        IMFMediaType_Release(type);
    }
    for (int i = 0; i < 50; i++)
    {
        DWORD index, flags = 0, length = 0;
        LONGLONG time = 0;
        IMFSample *sample = NULL;
        hr = IMFSourceReader_ReadSample(reader, stream, 0, &index, &flags, &time, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
        if (!sample) continue;
        IMFSample_GetTotalLength(sample, &length);
        IMFSample_Release(sample);
        if (size)
            printf("PASS %s: %ux%u, first frame at %.3f s, %lu bytes\n", what, (UINT)(size >> 32), (UINT)size,
                   time / 1e7, length);
        else
            printf("PASS %s: first samples at %.3f s, %lu bytes\n", what, time / 1e7, length);
        return 0;
    }
    printf("FAIL %s: nothing decoded (hr %#lx)\n", what, hr);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: mfplay.exe <video file>\n");
        return 2;
    }
    WCHAR path[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, path, MAX_PATH);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    IMFSourceReader *reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    if (FAILED(hr))
    {
        printf("FAIL open %s: %#lx\n", argv[1], hr);
        return 1;
    }

    int failures = 0, streams = 0;
    IMFMediaType *native;
    if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)))
    {
        IMFMediaType_Release(native);
        failures += decode(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &MFMediaType_Video, &MFVideoFormat_NV12, "video");
        streams++;
    }
    else
        printf("     (no video stream)\n");
    if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native)))
    {
        IMFMediaType_Release(native);
        failures += decode(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &MFMediaType_Audio, &MFAudioFormat_PCM, "audio");
        streams++;
    }
    else
        printf("     (no audio stream)\n");
    if (!streams)
    {
        printf("FAIL %s: no video or audio stream\n", argv[1]);
        failures++;
    }

    IMFSourceReader_Release(reader);
    MFShutdown();
    return failures;
}
