// Exercise the real pinned C entry points: reopening must revoke borrowed storage.
#include <PNGdec.h>

#include <png.inl>

static void need(int value, const char* message) {
  if (!value) {
    fprintf(stderr, "FAILED %s\n", message);
    exit(3);
  }
}

static int draw(PNGDRAW* image) {
  (void)image;
  return 1;
}

static void unbound(PNGIMAGE* png) {
  need(!png->pInflateState && !png->pInflateWindow && !png->ucPixels && !png->iPixelBufferSize,
       "C open discarded all bindings");
  need(DecodePNG(png, NULL, 0) == PNG_NO_BUFFER, "C decode requires a fresh binding");
}

int main(int argc, char** argv) {
  need(argc == 2, "fixture argument");
  FILE* input = fopen(argv[1], "rb");
  need(input != NULL, "fixture open");
  need(fseek(input, 0, SEEK_END) == 0, "fixture end");
  long length = ftell(input);
  need(length > 0 && length <= 65536, "bounded fixture");
  rewind(input);
  uint8_t* bytes = malloc((size_t)length);
  need(bytes && fread(bytes, 1, (size_t)length, input) == (size_t)length, "fixture read");
  fclose(input);
  PNGIMAGE png = {0};
  _Alignas(16) static uint8_t state[PNG_DECODE_STATE_BYTES];
  _Alignas(16) static uint8_t window[PNG_DECODE_WINDOW_BYTES];
  _Alignas(16) static uint8_t rows[(160 + 1) * 2 + 32];
  for (int repeat = 0; repeat < 2; ++repeat) {
    need(PNG_openRAM(&png, bytes, (int)length, draw) == PNG_SUCCESS, "C RAM open/reopen");
    unbound(&png);
    need(PNG_setDecodeWorkspace(&png, state, sizeof(state), window, sizeof(window), rows, sizeof(rows)), "C RAM bind");
    need(DecodePNG(&png, NULL, PNG_CHECK_CRC) == PNG_SUCCESS, "C RAM decode");
  }
  PNG_close(&png);
  unbound(&png);
  for (int repeat = 0; repeat < 2; ++repeat) {
    need(PNG_openFile(&png, argv[1], draw) == PNG_SUCCESS, "C file open/reopen");
    unbound(&png);
    need(PNG_setDecodeWorkspace(&png, state, sizeof(state), window, sizeof(window), rows, sizeof(rows)), "C file bind");
    need(DecodePNG(&png, NULL, PNG_CHECK_CRC) == PNG_SUCCESS, "C file decode");
    // Close only the file between reopen attempts, retaining the binding to test
    // the open entry itself rather than relying on PNG_close to invalidate it.
    if (repeat == 0) {
      fclose((FILE*)png.PNGFile.fHandle);
      png.PNGFile.fHandle = NULL;
    }
  }
  PNG_close(&png);
  unbound(&png);
  free(bytes);
  puts("C workspace openRAM/openFile reopen and decode: PASS");
  return 0;
}
