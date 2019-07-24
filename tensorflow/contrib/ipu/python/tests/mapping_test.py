# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import json

from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.contrib import ipu
from tensorflow.python.client import session as sl
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.platform import googletest


class MappingTest(test_util.TensorFlowTestCase):
  def testGather(self):
    def my_net(w, i):
      out = array_ops.gather(w, i)
      return [out]

    with ops.device('cpu'):
      i = array_ops.placeholder(np.int32, [256])
      w = array_ops.placeholder(np.float32, [1024, 8])
      report = gen_ipu_ops.ipu_event_trace()

    with ipu.ops.ipu_scope("/device:IPU:0"):
      r = ipu.ipu_compiler.compile(my_net, inputs=[w, i])

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)
    with sl.Session() as sess:
      i_h = np.arange(0, 3 * 256, 3)
      w_h = np.arange(8192).reshape(1024, 8)
      expect = np.take(w_h, i_h, axis=0)

      result = sess.run(r, {i: i_h, w: w_h})
      self.assertAllClose(result[0], expect)

      rep = sess.run(report)

      events = ipu.utils.extract_all_events(rep)

      for e in events:
        if e.type == IpuTraceEvent.COMPILE_END:
          j = e.compile_end.tensor_map.decode('utf-8')
          if len(j) > 0:
            tm = json.loads(e.compile_end.tensor_map.decode('utf-8'))

            bad_maps = []
            for g in tm['mappings']:
              for tensor in tm['mappings'][g]:
                # Total elements > 16
                if tensor[6] > 16:
                  # Tiles used == 1 and is_constant == 0
                  if len(tensor[7]) == 1 and tensor[4] == 0:
                    bad_maps += [tensor[0]]

      self.assertEqual(bad_maps, [])

  def testMappingJson(self):
    def my_net(a, b, c):
      a = array_ops.broadcast_to(a, shape=[1024])
      b = array_ops.strided_slice(b, [0], [8192], [8])
      c = array_ops.pad(c, paddings=[[256, 256]])
      out = a + b + c
      return [out]

    with ops.device('cpu'):
      a = array_ops.placeholder(np.float32, [])
      b = array_ops.placeholder(np.float32, [8192])
      c = array_ops.placeholder(np.float32, [512])
      report = gen_ipu_ops.ipu_event_trace()

    with ipu.ops.ipu_scope("/device:IPU:0"):
      r = ipu.ipu_compiler.compile(my_net, inputs=[a, b, c])

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)
    with sl.Session() as sess:

      fd = {a: 1.0, b: np.ones([8192]), c: np.ones([512])}
      result = sess.run(r, fd)

      expected = [2] * 256 + [3] * 512 + [2] * 256
      self.assertAllClose(result[0], expected)

      rep = sess.run(report)

      events = ipu.utils.extract_all_events(rep)

      for e in events:
        if e.type == IpuTraceEvent.COMPILE_END:
          j = e.compile_end.tensor_map.decode('utf-8')
          if len(j) > 0:
            tm = json.loads(e.compile_end.tensor_map.decode('utf-8'))
            # There are two fusions in the graph, zero pad and implicit
            # broadcast add. We work out which one's which by looking at
            # layouts.
            fusion_0_layout = []
            fusion_1_layout = []
            slice_layout = []
            add_layout = []
            for g in tm['mappings']:
              for tensor in tm['mappings'][g]:
                if tensor[0].startswith('fusion.'):
                  fusion_1_layout = tensor
                elif tensor[0].startswith('fusion'):
                  fusion_0_layout = tensor
                elif tensor[0].startswith('slice'):
                  slice_layout = tensor
                elif tensor[0].startswith('add'):
                  add_layout = tensor

            # The slice contains 4 elements on 256 tiles
            self.assertEqual(len(slice_layout[7]), 256)
            for tile in range(256):
              self.assertEqual(slice_layout[7][tile], [tile, 4])

            # The broadcast add will have the same layout as the slice as it
            # should be done inplace.
            if slice_layout[7] == fusion_1_layout[7]:
              pad_layout = fusion_0_layout
            else:
              self.assertEqual(slice_layout[7], fusion_0_layout[7])
              pad_layout = fusion_1_layout

            # The pad contains 512 elements on tile 0,
            # and one region with 32 elements on tiles 256-271
            self.assertEqual(len(pad_layout[7]), 17)
            self.assertEqual(pad_layout[7][0], [0, 512])
            for idx in range(1, 16):
              self.assertEqual(pad_layout[7][idx], [256 + idx, 32])

            # The add is done inplace
            self.assertEqual(slice_layout[7], add_layout[7])


if __name__ == "__main__":
  googletest.main()
