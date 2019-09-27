# Copyright 2017, 2019 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import numpy as np

from tensorflow.compiler.tests import xla_test
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.python import ipu
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import googletest


def count_event_type(events, event_type):
  return sum([1 if x.type == event_type else 0 for x in events])


class WhileLoopPerfTest(xla_test.XLATestCase):
  def testIpuWhilePerfTest(self):
    with self.session() as sess:

      def cond(i, v):
        del v
        return math_ops.less(i, 15)

      def body(i, v):
        v = v + i
        i = i + 1
        return (i, v)

      def my_net(v):
        i = constant_op.constant(0)
        r = control_flow_ops.while_loop(cond,
                                        body, (i, v),
                                        maximum_iterations=10)
        return [r[1]]

      with ops.device('cpu'):
        v = array_ops.placeholder(np.int32, [500])
        report = gen_ipu_ops.ipu_event_trace()

      with ipu.scopes.ipu_scope("/device:IPU:0"):
        r = ipu.ipu_compiler.compile(my_net, inputs=[v])

      cfg = ipu.utils.create_ipu_config(profiling=True, profile_execution=True)
      cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
      ipu.utils.configure_ipu_system(cfg)

      result = sess.run(r, {v: np.zeros([500], np.int32)})
      self.assertAllClose(result[0], np.broadcast_to(45, [500]))

      rep = sess.run(report)

      # Check that there is only one real compile
      reps = ipu.utils.extract_compile_reports(rep)
      self.assertEqual(len(reps), 1)

      # Check that there is only one execute
      reps = ipu.utils.extract_execute_reports(rep)
      self.assertEqual(len(reps), 1)


if __name__ == "__main__":
  os.environ['TF_XLA_FLAGS'] = ('--tf_xla_min_cluster_size=1 ' +
                                os.environ.get('TF_XLA_FLAGS', ''))
  googletest.main()
