"""用 ctypes 调系统自带的 libzstd、liblz4（镜像里已有，不需要 Python 包）。"""
from __future__ import annotations

import ctypes

PAGE = 4096

_zstd = ctypes.CDLL("libzstd.so.1")
_lz4 = ctypes.CDLL("liblz4.so.1")
_zstd.ZSTD_compressBound.restype = ctypes.c_size_t
_zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
_zstd.ZSTD_isError.restype = ctypes.c_uint
_zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
_zstd.ZSTD_getErrorName.restype = ctypes.c_char_p
_zstd.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
_zstd.ZSTD_createCCtx.restype = ctypes.c_void_p
_zstd.ZSTD_freeCCtx.argtypes = [ctypes.c_void_p]
_zstd.ZSTD_CCtx_reset.restype = ctypes.c_size_t
_zstd.ZSTD_CCtx_reset.argtypes = [ctypes.c_void_p, ctypes.c_int]
_zstd.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t
_zstd.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_zstd.ZSTD_CCtx_refPrefix.restype = ctypes.c_size_t
_zstd.ZSTD_CCtx_refPrefix.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
_zstd.ZSTD_compress2.restype = ctypes.c_size_t
_zstd.ZSTD_compress2.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.c_char_p, ctypes.c_size_t]
_lz4.LZ4_compress_default.restype = ctypes.c_int
_lz4.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

# zstd.h
ZSTD_c_compressionLevel = 100
ZSTD_c_windowLog = 101
ZSTD_c_enableLongDistanceMatching = 160
ZSTD_reset_session_and_parameters = 3


def _check(code):
    if _zstd.ZSTD_isError(code):
        raise RuntimeError(_zstd.ZSTD_getErrorName(code).decode())
    return code


class Zstd:
    """level 级别；window_log>0 时开长距离匹配，窗口 2^window_log 字节。"""

    def __init__(self, level=1, window_log=0):
        self.cctx = _zstd.ZSTD_createCCtx()
        self.level, self.window_log = level, window_log
        self.buf = None

    def _params(self):
        _check(_zstd.ZSTD_CCtx_reset(self.cctx, ZSTD_reset_session_and_parameters))
        _check(_zstd.ZSTD_CCtx_setParameter(self.cctx, ZSTD_c_compressionLevel, self.level))
        if self.window_log:
            _check(_zstd.ZSTD_CCtx_setParameter(self.cctx, ZSTD_c_windowLog, self.window_log))
            _check(_zstd.ZSTD_CCtx_setParameter(self.cctx, ZSTD_c_enableLongDistanceMatching, 1))

    def size(self, data: bytes, prefix: bytes | None = None) -> int:
        """压缩后的字节数；prefix 是参考数据（相当于 zstd --patch-from）。"""
        cap = _zstd.ZSTD_compressBound(len(data))
        if self.buf is None or len(self.buf) < cap:
            self.buf = ctypes.create_string_buffer(cap)
        self._params()
        if prefix:
            _check(_zstd.ZSTD_CCtx_refPrefix(self.cctx, prefix, len(prefix)))
        return _check(_zstd.ZSTD_compress2(self.cctx, self.buf, cap, data, len(data)))

    def close(self):
        _zstd.ZSTD_freeCCtx(self.cctx)


_lz4_buf = ctypes.create_string_buffer(PAGE + PAGE // 255 + 32)


def lz4_size(page: bytes) -> int:
    return _lz4.LZ4_compress_default(page, _lz4_buf, len(page), len(_lz4_buf))


def zram_size(compressed: int) -> int:
    """zram 存不下的页（压缩后超过约 3/4 页）按原样存。"""
    return PAGE if compressed > PAGE * 3 // 4 else compressed
