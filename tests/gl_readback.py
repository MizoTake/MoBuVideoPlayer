"""Read Story textures without depending on the host's pixel pack state."""

import ctypes


TEXTURE_2D = 0x0DE1
TEXTURE_BINDING_2D = 0x8069
PIXEL_PACK_BUFFER = 0x88EB
PIXEL_PACK_BUFFER_BINDING = 0x88ED
PACK_PARAMETERS = {
    'alignment': 0x0D05,
    'row_length': 0x0D02,
    'skip_rows': 0x0D03,
    'skip_pixels': 0x0D04,
    'image_height': 0x806C,
    'skip_images': 0x806B,
}


class TextureReader:
    def __init__(self, gl, bind_buffer=None):
        self.gl = gl
        self.bind_buffer = bind_buffer

    def integer(self, name):
        value = ctypes.c_int()
        self.gl.glGetIntegerv(name, ctypes.byref(value))
        return value.value

    def read(self, texture_id):
        gl = self.gl
        result = {'gl_id': texture_id}
        if not texture_id or not gl.glIsTexture(texture_id):
            return result
        if self.bind_buffer is None:
            # Extension addresses are unavailable until the render callback has a current context.
            gl.wglGetProcAddress.argtypes = [ctypes.c_char_p]
            gl.wglGetProcAddress.restype = ctypes.c_void_p
            address = gl.wglGetProcAddress(b'glBindBuffer')
            invalid = (None, 0, 1, 2, 3, (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1)
            if address in invalid:
                raise RuntimeError('OpenGL pixel pack buffer binding is unavailable.')
            self.bind_buffer = ctypes.WINFUNCTYPE(None, ctypes.c_uint, ctypes.c_uint)(address)
            gl.glPixelStorei.argtypes = [ctypes.c_uint, ctypes.c_int]
            gl.glPixelStorei.restype = None
        prior_texture = self.integer(TEXTURE_BINDING_2D)
        prior_buffer = self.integer(PIXEL_PACK_BUFFER_BINDING)
        prior_pack = {name: self.integer(value) for name, value in PACK_PARAMETERS.items()}
        result.update(pack_before=prior_pack, pixel_pack_buffer_before=prior_buffer)
        width, height = ctypes.c_int(), ctypes.c_int()
        try:
            # Host row strides and skips can exceed the tightly sized CPU array even when its dimensions are correct.
            self.bind_buffer(PIXEL_PACK_BUFFER, 0)
            for name, value in PACK_PARAMETERS.items():
                gl.glPixelStorei(value, 1 if name == 'alignment' else 0)
            gl.glBindTexture(TEXTURE_2D, texture_id)
            gl.glGetTexLevelParameteriv(TEXTURE_2D, 0, 0x1000, ctypes.byref(width))
            gl.glGetTexLevelParameteriv(TEXTURE_2D, 0, 0x1001, ctypes.byref(height))
            result.update(width=width.value, height=height.value)
            if 0 < width.value < 4096 and 0 < height.value < 4096:
                data = (ctypes.c_ubyte * (width.value * height.value * 4))()
                gl.glGetTexImage(TEXTURE_2D, 0, 0x80E1, 0x1401, data)
                center = (height.value // 2 * width.value + width.value // 2) * 4
                result['bgra'] = list(data[center:center + 4])
        finally:
            gl.glBindTexture(TEXTURE_2D, prior_texture)
            for name, value in PACK_PARAMETERS.items():
                gl.glPixelStorei(value, prior_pack[name])
            self.bind_buffer(PIXEL_PACK_BUFFER, prior_buffer)
        return result
