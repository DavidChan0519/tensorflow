# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import json

from tensorflow.compiler.tests import xla_test
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.python import ipu
from tensorflow.python.client import session as sl
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.platform import googletest


class MappingTest(xla_test.XLATestCase):
  def testGather(self):
    def my_net(w, i):
      w = ipu.ops.internal_ops.remap(w)
      i = ipu.ops.internal_ops.remap(i)
      out = array_ops.gather(w, i)
      return [out]

    with ops.device('cpu'):
      i = array_ops.placeholder(np.int32, [8])
      w = array_ops.placeholder(np.float32, [32 * 1024])
      report = gen_ipu_ops.ipu_event_trace()

    with ipu.scopes.ipu_scope("/device:IPU:0"):
      r = ipu.ipu_compiler.compile(my_net, inputs=[w, i])

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)
    with sl.Session() as sess:
      i_h = np.arange(0, 8)
      w_h = np.arange(32 * 1024)

      result = sess.run(r, {i: i_h, w: w_h})
      self.assertAllClose(result[0], np.take(w_h, i_h))

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
                  # Tiles used != 1024
                  if len(tensor[7]) != 1024:
                    bad_maps += [tensor[0]]

      self.assertEqual(len(bad_maps), 0)


if __name__ == "__main__":
  googletest.main()
