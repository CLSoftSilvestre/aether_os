/* jconfig.h — AetherOS AArch64 bare-metal configuration for libjpeg 9f.
 * Hand-crafted replacement for the ./configure-generated file. */

#define HAVE_PROTOTYPES      1
#define HAVE_UNSIGNED_CHAR   1
#define HAVE_UNSIGNED_SHORT  1
#define HAVE_STDDEF_H        1
#define HAVE_STDLIB_H        1
#undef  NEED_BSD_STRINGS
#undef  NEED_SYS_TYPES_H
#undef  NEED_FAR_POINTERS
#undef  NEED_SHORT_EXTERNAL_NAMES
#undef  INCOMPLETE_TYPES_BROKEN

#ifdef  JPEG_INTERNALS
#undef  RIGHT_SHIFT_IS_UNSIGNED
#endif

#ifdef  JPEG_CJPEG_DJPEG
#define BMP_SUPPORTED
#define GIF_SUPPORTED
#define PPM_SUPPORTED
#undef  RLE_SUPPORTED
#define TARGA_SUPPORTED
#define TWO_FILE_COMMANDLINE
#undef  NEED_SIGNAL_CATCHER
#undef  DONT_USE_B_MODE
#undef  PROGRESS_REPORT
#endif
