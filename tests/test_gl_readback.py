"""Check readback memory bounds and host state with nondefault OpenGL state."""

import ctypes
import unittest

from gl_readback import PACK_PARAMETERS, PIXEL_PACK_BUFFER_BINDING, TEXTURE_BINDING_2D, TextureReader


class FakeGl:
    def __init__(self):
        self.state = {value: 0 for value in PACK_PARAMETERS.values()}
        self.state.update({PACK_PARAMETERS['alignment']: 4, PIXEL_PACK_BUFFER_BINDING: 0,
                           TEXTURE_BINDING_2D: 71})
        self.width, self.height = 2, 2
        self.read_error = False
        self.reads = []

    def dirty_pack_state(self):
        self.state.update(dict(zip(PACK_PARAMETERS.values(), (8, 13, 3, 5, 7, 11))))

    def glIsTexture(self, texture):
        return texture == 11

    def glGetIntegerv(self, name, destination):
        ctypes.cast(destination, ctypes.POINTER(ctypes.c_int))[0] = self.state[name]

    def glBindTexture(self, target, texture):
        self.state[TEXTURE_BINDING_2D] = texture

    def bind_buffer(self, target, buffer):
        self.state[PIXEL_PACK_BUFFER_BINDING] = buffer

    def glPixelStorei(self, name, value):
        self.state[name] = value

    def glGetTexLevelParameteriv(self, target, level, name, destination):
        ctypes.cast(destination, ctypes.POINTER(ctypes.c_int))[0] = self.width if name == 0x1000 else self.height

    def glGetTexImage(self, target, level, format_, type_, destination):
        self.reads.append(dict(self.state))
        if self.state[PIXEL_PACK_BUFFER_BINDING]:
            raise BufferError('The CPU address would be interpreted as a pixel pack buffer offset.')
        row_pixels = self.state[PACK_PARAMETERS['row_length']] or self.width
        alignment = self.state[PACK_PARAMETERS['alignment']]
        stride = (row_pixels * 4 + alignment - 1) // alignment * alignment
        start = self.state[PACK_PARAMETERS['skip_rows']] * stride + self.state[PACK_PARAMETERS['skip_pixels']] * 4
        required = start + (self.height - 1) * stride + self.width * 4
        if required > ctypes.sizeof(destination):
            raise BufferError('The active pixel pack state would write beyond the CPU buffer.')
        if self.read_error:
            raise OSError('Injected readback failure')
        for row in range(self.height):
            for column in range(self.width):
                offset = start + row * stride + column * 4
                for channel, value in enumerate((20, 210, 235, 255)):
                    destination[offset + channel] = value


class TextureReadbackTests(unittest.TestCase):
    def test_nondefault_row_and_skip_settings_cannot_overrun_the_cpu_buffer(self):
        gl = FakeGl()
        gl.dirty_pack_state()
        result = TextureReader(gl, gl.bind_buffer).read(11)
        self.assertEqual(result['bgra'], [20, 210, 235, 255])
        self.assertEqual(len(gl.reads), 1)

    def test_bound_pixel_pack_buffer_does_not_reinterpret_the_cpu_pointer(self):
        gl = FakeGl()
        gl.state[PIXEL_PACK_BUFFER_BINDING] = 17
        result = TextureReader(gl, gl.bind_buffer).read(11)
        self.assertEqual(result['bgra'], [20, 210, 235, 255])
        self.assertEqual(gl.reads[0][PIXEL_PACK_BUFFER_BINDING], 0)

    def test_success_restores_all_pack_parameters_and_bindings(self):
        gl = FakeGl()
        gl.dirty_pack_state()
        gl.state[PIXEL_PACK_BUFFER_BINDING] = 17
        before = dict(gl.state)
        TextureReader(gl, gl.bind_buffer).read(11)
        self.assertEqual(gl.state, before)

    def test_readback_failure_restores_all_pack_parameters_and_bindings(self):
        gl = FakeGl()
        gl.dirty_pack_state()
        gl.state[PIXEL_PACK_BUFFER_BINDING] = 17
        gl.read_error = True
        before = dict(gl.state)
        with self.assertRaisesRegex(OSError, 'Injected readback failure'):
            TextureReader(gl, gl.bind_buffer).read(11)
        self.assertEqual(gl.state, before)

    def test_report_preserves_the_original_pack_state_for_host_diagnostics(self):
        gl = FakeGl()
        gl.dirty_pack_state()
        gl.state[PIXEL_PACK_BUFFER_BINDING] = 17
        before = dict(gl.state)
        result = TextureReader(gl, gl.bind_buffer).read(11)
        self.assertEqual(result['pack_before'], {name: before[value] for name, value in PACK_PARAMETERS.items()})
        self.assertEqual(result['pixel_pack_buffer_before'], 17)

    def test_invalid_texture_does_not_read_or_change_host_state(self):
        gl = FakeGl()
        before = dict(gl.state)
        self.assertEqual(TextureReader(gl, gl.bind_buffer).read(0), {'gl_id': 0})
        self.assertEqual(gl.state, before)
        self.assertEqual(gl.reads, [])

    def test_invalid_dimensions_do_not_read_or_leave_texture_bound(self):
        gl = FakeGl()
        gl.width = 0
        before = dict(gl.state)
        result = TextureReader(gl, gl.bind_buffer).read(11)
        self.assertNotIn('bgra', result)
        self.assertEqual(gl.state, before)
        self.assertEqual(gl.reads, [])


if __name__ == '__main__':
    unittest.main()
