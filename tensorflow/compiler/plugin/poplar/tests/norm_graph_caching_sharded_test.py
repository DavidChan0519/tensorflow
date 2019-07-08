# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import test_utils as tu

from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.python.platform import googletest
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.layers import convolutional
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.training import gradient_descent
from tensorflow.python.layers import normalization as layers_norm


class NormGraphCachingTest(test_util.TensorFlowTestCase):
  def testBatchNormsMatchFwdBwdSomeOnShard0SomeOnShard1(self):
    with ops.device("/device:IPU:0"):
      x = array_ops.placeholder(np.float32, shape=[1, 4, 4, 2])

      with variable_scope.variable_scope("vs", use_resource=True):
        with tu.ipu_shard(0):
          y = convolutional.conv2d(
              x,
              2,
              1,
              use_bias=False,
              kernel_initializer=init_ops.ones_initializer(),
              name='conv1')
          y = layers_norm.batch_normalization(y, fused=True, training=True)
          y = convolutional.conv2d(
              y,
              2,
              1,
              use_bias=False,
              kernel_initializer=init_ops.ones_initializer(),
              name='conv2')
          y = layers_norm.batch_normalization(y, fused=True, training=True)

        with tu.ipu_shard(1):
          y = convolutional.conv2d(
              y,
              2,
              1,
              use_bias=False,
              kernel_initializer=init_ops.ones_initializer(),
              name='conv3')
          y = layers_norm.batch_normalization(y, fused=True, training=True)

      loss = math_ops.reduce_sum(y)
      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train = optimizer.minimize(loss)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(True, True, True, sharded=True)
    tu.move_variable_initialization_to_cpu()
    with tu.ipu_session() as sess:
      sess.run(variables.global_variables_initializer())

      sess.run(report)

      sess.run([train, loss], {x: np.zeros([1, 4, 4, 2])})

      result = sess.run(report)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      # Two BN for forwards (on shards 0 and 1) and two BN for grad
      # (note that we don't cache gradient application)
      ok = [
          '__seed*',
          '*OnTileCopy*',
          'Copy_',
          'vs/conv1/Conv2D/convolution.*/Conv_1x1',
          'vs/conv3/Conv2D/convolution.*/Conv_1x1',
          'vs/batch_normalization/FusedBatchNorm*/batch-norm-training.*/',
          'vs/batch_normalization_2/FusedBatchNorm*/batch-norm-training.*/',
          'Sum/reduce.*/ReduceFinalStage/IntermediateToOutput/Reduce',
          'gradients/vs/batch_normalization_2/FusedBatchNorm*_grad/FusedBatchNormGrad*/batch-norm-grad.*/',
          'gradients/vs/batch_normalization_1/FusedBatchNorm*_grad/FusedBatchNormGrad*/batch-norm-grad.*/',
          'GradientDescent/update_vs/batch_normalization/',
          'GradientDescent/update_vs/batch_normalization_1/',
          'GradientDescent/update_vs/batch_normalization_2/',
          'gradients/vs/conv3/Conv2D_grad/Conv2DBackpropFilter/fusion.*/Conv_4x4',
          'gradients/vs/conv3/Conv2D_grad/Conv2DBackpropFilter/fusion.*/AddTo',
          'gradients/vs/conv2/Conv2D_grad/Conv2DBackpropFilter/fusion.*/Conv_4x4',
          'gradients/vs/conv2/Conv2D_grad/Conv2DBackpropFilter/fusion.*/AddTo',
      ]

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))


if __name__ == "__main__":
  googletest.main()
