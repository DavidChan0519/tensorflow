from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import numpy as np
import test_utils as tu

from tensorflow.compiler.tests import xla_test
from tensorflow.python.platform import googletest
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_array_ops
from tensorflow.python.ops import math_ops
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops


class IpuFuseOpsTest(xla_test.XLATestCase):
  def testReductionSumVectorF16NoConverts(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [4096], name="a")
        output = math_ops.reduce_sum(pa, axis=[0])

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)
      fd = {pa: np.ones([4096])}
      result = sess.run(output, fd)
      self.assertAllClose(result, 4096)

      result = sess.run(report)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      # Check that there are no casts to float at the beginning
      # Note that intermidiates are still floats, so there is a final cast
      ok = [
          '__seed*', 'host-exchange-local-copy-',
          'Sum/reduce*/ReduceOnTile/InToIntermediateNoExchange/Reduce',
          'Sum/reduce*/ReduceStage*/IntermediateToIntermediate/Reduce',
          'Sum/reduce*/ReduceFinalStage/IntermediateToOutput/Reduce',
          'Sum/reduce*/ReduceFinalStage/Cast'
      ]

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testNoCastsF32ToF16ToF32(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3])
        b = math_ops.cast(pa, np.float16)
        c = math_ops.cast(b, np.float32)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: [2.0, 0.5, 1.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [2.0, 0.5, 1.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 2)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      self.assertTrue(len(cs_list) == 0)

  def testNoCastsF16ReduceWithReshape(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3, 4])
        a = gen_array_ops.reshape(pa, [4, 3])
        a = math_ops.reduce_sum(a, axis=(1))

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: np.ones([3, 4])}
      result = sess.run(a, fd)
      self.assertAllClose(result, [3.0, 3.0, 3.0, 3.0])

      result = sess.run(report)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'Sum/reduce*/Reduce',
      ]

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testMultipleReduces(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        pb = array_ops.placeholder(np.float16, [3])
        a = math_ops.cast(pa, np.float32)
        a = math_ops.reduce_sum(a)
        a = math_ops.cast(a, np.float16)
        b = math_ops.cast(pb, np.float32)
        b = math_ops.reduce_sum(b)
        b = math_ops.cast(b, np.float16)
        c = a + b

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: [2.0, 0.5, 1.0], pb: [1.0, 1.0, 2.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, 7.5)

      result = sess.run(report)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*', 'host-exchange-local-copy-', 'Sum/reduce*/Reduce',
          'Sum_1/reduce*/Reduce', 'add/add*/AddTo'
      ]

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testNoCastsF16ToF32ToF16(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        b = math_ops.cast(pa, np.float32)
        c = math_ops.cast(b, np.float16)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)
      fd = {pa: [2.0, 0.5, 1.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [2.0, 0.5, 1.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 2)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      self.assertTrue(len(cs_list) == 0)

  def testDontRemoveCastsIfUsed(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        b = math_ops.cast(pa, np.float32)
        const = array_ops.constant(1.0, np.float32)
        b = b + const
        c = math_ops.cast(b, np.float16)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: [2.0, 0.5, 1.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [3.0, 1.5, 2.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*', 'host-exchange-local-copy-', 'Cast/convert.*/Cast',
          'add/fusion*/Add', 'Cast_1/convert.*/Cast'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))


if __name__ == "__main__":
  os.environ['TF_XLA_FLAGS'] = (
      '--tf_xla_min_cluster_size=2 ' + os.environ.get('TF_XLA_FLAGS', ''))
  googletest.main()
