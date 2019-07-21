# Copyright 2017, 2018, 2019 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import numpy as np
import test_utils as tu

from tensorflow.compiler.tests import xla_test
from tensorflow.python.platform import googletest
from tensorflow.python.framework import ops
from tensorflow.python.layers import normalization as layers_norm
from tensorflow.python.keras import layers
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_nn_ops
from tensorflow.python.ops import gen_math_ops
from tensorflow.python.ops import gradients_impl
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.training import gradient_descent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.python.compiler.xla import xla


class IpuFuseOpsTest(xla_test.XLATestCase):
  def testSigmoid(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="a")
        c = math_ops.sigmoid(pa)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [-6.0, 0.0, 6.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0.002473, 0.5, 0.997527])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'Sigmoid/custom-call/Nonlinearity']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testSigmoidNotInplace(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="a")
        c = math_ops.sigmoid(pa) + pa

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [-6.0, 0.0, 6.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [-5.997527, 0.5, 6.997527])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'Copy_XLA_Args/arg0.*_to_Sigmoid/custom-call/Nonlinearity/out/OnTileCopy-0',
          'Sigmoid/custom-call/Nonlinearity', 'add/add.*/AddTo'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testSigmoidGrad(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="grad")
        pb = array_ops.placeholder(np.float32, [3], name="in")
        c = gen_math_ops.sigmoid_grad(pa, pb)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [-1.0, 1.0, 6.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [2.0, 0.25, 0.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'SigmoidGrad/custom-call/NonLinearityGrad']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testRelu(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="a")
        c = nn_ops.relu(pa)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: [-6.0, 0.0, 6.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0.0, 0.0, 6.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'Relu/custom-call/Nonlinearity']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testReluNotInPlace(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="a")
        c = nn_ops.relu(pa) + pa

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      fd = {pa: [1, -2, 1]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [2, -2, 2])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'Copy_XLA_Args/arg0.*_to_Relu/custom-call/Nonlinearity/out/OnTileCopy-0',
          'Relu/custom-call/Nonlinearity', 'add/add.*/AddTo'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testReluNotInPlace2(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [5], name="a")
      b = array_ops.concat([pa, pa], axis=0)
      c = nn_ops.relu(b)

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system()

    with tu.ipu_session() as sess:
      fd = {pa: [-2, -1, 0, 1, 2]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0, 0, 0, 1, 2, 0, 0, 0, 1, 2])
      self.assertTrue(len(result) == 10)

      result_report = sess.run(report)

      s = tu.extract_all_strings_from_event_trace(result_report)
      cs_list = tu.get_compute_sets_from_report(s)
      ok = [
          '__seed*',
          'Copy_XLA_Args/arg0.*_to_Relu/custom-call/Nonlinearity/out/OnTileCopy-0',
          'Relu/custom-call/Nonlinearity'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testReluGrad(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [3], name="grad")
        pb = array_ops.placeholder(np.float32, [3], name="in")
        c = gen_nn_ops.relu_grad(pa, pb)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [-1.0, 1.0, 6.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0.0, 0.5, 1.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'ReluGrad/custom-call/NonLinearityGrad']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testMaxPool(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [1, 1, 10, 10], name="a")
        c = nn.max_pool(
            pa,
            ksize=[1, 1, 5, 5],
            strides=[1, 1, 2, 2],
            data_format='NCHW',
            padding='SAME',
            name="max")

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {
          pa: np.ones([1, 1, 10, 10]),
      }
      result = sess.run(c, fd)
      self.assertAllClose(result, np.ones([1, 1, 5, 5]))

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'max/custom-call*/maxPool5x5']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testFwdAndBwdMaxPool(self):
    with self.session() as sess:
      input = np.arange(16).reshape(1, 4, 4, 1)
      output_grad = np.full((1, 2, 2, 1), 0.1)

      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, [1, 4, 4, 1], name="a")
        pb = array_ops.placeholder(np.float32, [1, 2, 2, 1], name="b")
        c = nn.max_pool(
            pa,
            ksize=[1, 2, 2, 1],
            strides=[1, 2, 2, 1],
            data_format='NCHW',
            padding='SAME')
        d = gen_nn_ops.max_pool_grad(
            pa,
            c,
            pb,
            ksize=[1, 2, 2, 1],
            strides=[1, 2, 2, 1],
            data_format='NCHW',
            padding='SAME')

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)
      fe = {
          pa: input,
          pb: output_grad,
      }
      output, input_grad = sess.run((c, d), fe)
      self.assertAllClose(output, [[[[5.], [7.]], [[13.], [15.]]]])
      self.assertAllClose(
          input_grad, [[[[0.], [0.], [0.], [0.]], [[0.], [0.1], [0.], [0.1]],
                        [[0.], [0.], [0.], [0.]], [[0.], [0.1], [0.], [0.1]]]])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*', 'Copy_*', 'MaxPool/custom-call*/maxPool2x2/',
          'MaxPoolGrad/custom-call*/maxPool2x2'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testScaledAddTo(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        pb = array_ops.placeholder(np.float16, [3])
        const = array_ops.constant(2.0, np.float16)
        c = pa + pb * const

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [1.0, 2.0, 3.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [4.0, 4.5, 7.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'host-exchange-local-copy-', 'add/fusion/AddTo']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testScaledSubtractFrom(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        pb = array_ops.placeholder(np.float16, [3])
        const = array_ops.constant(2.0, np.float16)
        # note how const operand index varies compared to testScaledAddTo
        # still should match as it will be reordered
        c = pa - const * pb

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [1.0, 2.0, 3.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0.0, -3.5, -5.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'host-exchange-local-copy-', 'sub/fusion/AddTo']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testScaledAddToVariable(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        pb = array_ops.placeholder(np.float16, [3])
        pc = array_ops.placeholder(np.float16, [1])
        c = pa + pb * pc

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [1.0, 2.0, 3.0], pc: [2.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [4.0, 4.5, 7.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'host-exchange-local-copy-', 'add/fusion/AddTo']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testScaledSubtractFromVariable(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float16, [3])
        pb = array_ops.placeholder(np.float16, [3])
        pc = array_ops.placeholder(np.float16, [1])
        c = pa - pc * pb

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(report)

      fd = {pa: [2.0, 0.5, 1.0], pb: [1.0, 2.0, 3.0], pc: [2.0]}
      result = sess.run(c, fd)
      self.assertAllClose(result, [0.0, -3.5, -5.0])

      result = sess.run(report)
      self.assertTrue(len(result) == 3)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'host-exchange-local-copy-', 'sub/fusion/AddTo']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testConvolutionBiasApply(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float32, shape=[1, 4, 4, 2])

        with variable_scope.variable_scope("vs", use_resource=True):
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer())(x)
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer())(y)

        loss = math_ops.reduce_sum(y)
        optimizer = gradient_descent.GradientDescentOptimizer(0.1)
        train = optimizer.minimize(loss)

        with ops.device('cpu'):
          report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      sess.run([train, loss], {x: np.zeros([1, 4, 4, 2])})

      result = sess.run(report)
      self.assertEqual(len(result),
                       6)  # 2xcompile, 1xupload, 1xload, 1xdownload, 1xexecute

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'GradientDescent/update_vs/conv2d/bias/ResourceApplyGradientDescent/fusion.*/Reduce'
      ]
      self.assertTrue(tu.check_compute_sets_in_whitelist_entries(cs_list, ok))

  def testConvolutionBiasApplyVariableLR(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float32, shape=[1, 4, 4, 2])
        lr = array_ops.placeholder(np.float32, shape=[])

        with variable_scope.variable_scope("vs", use_resource=True):
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer())(x)
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer())(y)

        loss = math_ops.reduce_sum(y)
        optimizer = gradient_descent.GradientDescentOptimizer(lr)
        train = optimizer.minimize(loss)

        with ops.device('cpu'):
          report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      sess.run([train, loss], {x: np.zeros([1, 4, 4, 2]), lr: 0.1})

      result = sess.run(report)
      self.assertEqual(len(result),
                       6)  # 2xcompile, 1xupload, 1xload, 1xdownload, 1xexecute

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*', 'Copy_', 'host-exchange-local-copy-',
          'vs/conv2d/BiasAdd/fusion*/Op/Add',
          'vs/conv2d_1/BiasAdd/fusion.2/Op/Add',
          'GradientDescent/update_vs/conv2d/bias/ResourceApplyGradientDescent/fusion.3/ReduceFinalStage/IntermediateToOutput/Reduce',
          'GradientDescent/update_vs/conv2d/bias/ResourceApplyGradientDescent/fusion*/negate/Op/Negate',
          'GradientDescent/update_vs/conv2d_1/bias/ResourceApplyGradientDescent/multiply*/Op/Multiply',
          'GradientDescent/update_vs/conv2d_1/bias/ResourceApplyGradientDescent/fusion*/AddTo',
          'vs/conv2d/BiasAdd/fusion*/Op/Add',
          'Sum/reduce*/ReduceFinalStage/IntermediateToOutput/Reduce',
          'gradients/vs/conv2d/Conv2D_grad/Conv2DBackpropFilter/fusion*/Conv_4x4/Transpose*',
          'gradients/vs/conv2d/Conv2D_grad/Conv2DBackpropFilter/fusion*/Conv_4x4/Convolve*',
          'gradients/vs/conv2d/Conv2D_grad/Conv2DBackpropFilter/fusion*/AddTo*',
          'vs/conv2d/Conv2D/convolution*/Conv_1x1'
      ]

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testAvgPoolValid(self):
    with self.session() as sess:
      np.random.seed(0)
      shape = [1, 10, 10, 1]
      data = np.random.uniform(0, 1, shape)
      # The expected answer was generated using TF on the cpu
      expected = [[[[0.47279388]]]]

      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, shape, name="a")
        output = nn.avg_pool(
            pa,
            ksize=[1, 10, 10, 1],
            strides=[1, 1, 1, 1],
            data_format='NHWC',
            padding='VALID',
            name="avg")

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      fd = {pa: data}
      result = sess.run(output, fd)
      self.assertAllClose(result, expected)

      result = sess.run(report)
      self.assertEqual(len(result), 4)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'avg/custom-call*/avgPool10x10']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testAvgPoolValidWithBroadcast(self):
    with self.session() as sess:
      np.random.seed(0)
      shape = [1, 10, 10, 1]
      data = np.random.uniform(0, 1, shape)
      # The expected answer was generated using TF on the cpu
      expected = [[[[0.52647954], [0.44196457], [0.49284577]],
                   [[0.44039682], [0.44067329], [0.44934618]],
                   [[0.46444583], [0.45419583], [0.38236427]]]]

      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, shape, name="a")
        output = nn.avg_pool(
            pa,
            ksize=[1, 5, 5, 1],
            strides=[1, 2, 2, 1],
            data_format='NHWC',
            padding='VALID',
            name="avg")

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      fd = {pa: data}
      result = sess.run(output, fd)
      self.assertAllClose(result, expected)

      result = sess.run(report)
      self.assertEqual(len(result), 4)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'avg/custom-call*/avgPool5x5']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testAvgPoolSameWithReshape(self):
    with self.session() as sess:
      np.random.seed(0)
      shape = [1, 10, 10, 1]
      data = np.random.uniform(0, 1, shape)
      # The expected answer was generated using TF on the cpu
      expected = [[[[0.64431685], [0.51738459], [0.49705142], [0.60235918],
                    [0.73694557]],
                   [[0.57755166], [0.47387227], [0.40451217], [0.4876942],
                    [0.55843753]],
                   [[0.49037799], [0.4466258], [0.35829377], [0.40070742],
                    [0.37205362]],
                   [[0.47563809], [0.4075647], [0.34894851], [0.35470542],
                    [0.3322109]],
                   [[0.52914065], [0.45464769], [0.38156652], [0.32455513],
                    [0.33199897]]]]

      with ops.device("/device:IPU:0"):
        pa = array_ops.placeholder(np.float32, shape, name="a")
        output = nn.avg_pool(
            pa,
            ksize=[1, 5, 5, 1],
            strides=[1, 2, 2, 1],
            data_format='NHWC',
            padding='SAME',
            name="avg")

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      fd = {pa: data}
      result = sess.run(output, fd)
      self.assertAllClose(result, expected)

      result = sess.run(report)
      self.assertEqual(len(result), 4)

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      ok = ['__seed*', 'avg/custom-call*/avgPool5x5']
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testFullyConnectedWithBias(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float32, shape=[2, 2])
        weights = array_ops.placeholder(np.float32, shape=[2, 2])
        bias = array_ops.placeholder(np.float32, shape=[2])
        x_new = nn.xw_plus_b(x, weights, bias)

        with ops.device('cpu'):
          report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(report)

      out = sess.run(x_new, {
          x: np.full([2, 2], 3),
          weights: np.full([2, 2], 4),
          bias: np.ones([2]),
      })
      self.assertAllClose(np.full([2, 2], 25), out)

      result = sess.run(report)
      self.assertEqual(len(result),
                       4)  # 1xcompile, 1xload, 1xdownload, 1xexecute

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      ok = [
          '__seed*', 'host-exchange-local-copy',
          'xw_plus_b/MatMul/dot.*/Conv_1/Convolve', 'xw_plus_b/fusion/Op/Add'
      ]
      self.assertTrue(tu.check_compute_sets_in_whitelist_entries(cs_list, ok))

  def testConvWithBnAndRelu(self):
    with self.session() as sess:
      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float32, shape=[1, 4, 4, 2])
        with variable_scope.variable_scope("vs", use_resource=True):
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer())(x)
          y = layers_norm.batch_normalization(y, fused=True)
          y = nn_ops.relu(y)

        with ops.device('cpu'):
          report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system(True, True, True)

      sess.run(variables.global_variables_initializer())

      sess.run(report)

      sess.run(y, {x: np.zeros([1, 4, 4, 2])})

      result = sess.run(report)
      self.assertEqual(len(result),
                       6)  # 2xcompile, 1xupload 1xload, 1xdownload, 1xexecute

      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*', 'host-exchange-local-copy', 'Copy_',
          'vs/conv2d/Conv2D/convolution.*/Conv_1x1', 'vs/conv2d/BiasAdd',
          'vs/batch_normalization/FusedBatchNorm*/batch-norm-inference.*/',
          'vs/Relu/custom-call/Nonlinearity'
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testBiasApplyFixedLR(self):
    with self.session() as sess:
      input = np.ones((1, 4, 4, 2))

      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float32, shape=[1, 4, 4, 2])

        with variable_scope.variable_scope("vs", use_resource=True):
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer(),
              bias_initializer=init_ops.ones_initializer(),
              name="a")(x)
          y = nn.relu(y)

        loss = math_ops.reduce_sum(y)
        optimizer = gradient_descent.GradientDescentOptimizer(0.1)
        train = optimizer.minimize(loss)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(variables.global_variables_initializer())
      sess.run(report)
      fe = {
          x: input,
      }
      l, _ = sess.run((loss, train), fe)
      tvars = variables.global_variables()
      tvars_vals = sess.run(tvars)

      found = False
      for var, val in zip(tvars, tvars_vals):
        if var.name == "vs/a/bias:0":
          # Value computed using the CPU backend
          self.assertAllClose(val, [-0.6, -0.6])
          found = True
      self.assertTrue(found)

  def testBiasApplyVariableLR(self):
    with self.session() as sess:
      input = np.ones((1, 4, 4, 2))

      with ops.device("/device:IPU:0"):
        x = array_ops.placeholder(np.float16, shape=[1, 4, 4, 2])
        lr = array_ops.placeholder(np.float16, shape=[])
        with variable_scope.variable_scope("vs", use_resource=True):
          y = layers.Conv2D(
              2,
              1,
              use_bias=True,
              kernel_initializer=init_ops.ones_initializer(),
              bias_initializer=init_ops.ones_initializer(),
              name="a")(x)
          y = nn.relu(y)

        loss = math_ops.reduce_sum(y)
        optimizer = gradient_descent.GradientDescentOptimizer(lr)
        train = optimizer.minimize(loss)

      with ops.device('cpu'):
        report = gen_ipu_ops.ipu_event_trace()

      tu.configure_ipu_system()

      sess.run(variables.global_variables_initializer())
      sess.run(report)
      fe = {
          x: input,
          lr: 0.1,
      }
      l, _ = sess.run((loss, train), fe)
      tvars = variables.global_variables()
      tvars_vals = sess.run(tvars)

      found = False
      for var, val in zip(tvars, tvars_vals):
        if var.name == "vs/a/bias:0":
          # Value computed using the CPU backend
          self.assertAllClose(val, [-0.6, -0.6], atol=0.001)
          found = True
      self.assertTrue(found)

  def testUnsortedSegmentSumConstLR(self):
    with self.session() as sess:

      def network(x, y1, y2):
        with variable_scope.variable_scope("vs", use_resource=True):
          w1 = variable_scope.get_variable(
              "w1",
              shape=[10, 200],
              dtype=np.float32,
              initializer=init_ops.constant_initializer(1))
          g1 = array_ops.gather(w1, y1)
          g2 = array_ops.gather(w1, y2)

          a = math_ops.reduce_sum(g1 + g2)

        optimizer = gradient_descent.GradientDescentOptimizer(0.1)
        grads = [a]
        grads = [
            gradients_impl.gradients(g, variables.trainable_variables())[0]
            for g in grads
        ]
        grads = [array_ops.expand_dims(g, 0) for g in grads]
        grad = array_ops.concat(grads, axis=0)
        grad = math_ops.reduce_mean(grad, 0)
        train = optimizer.apply_gradients([(grad, w1)])
        return a, train

      with ops.device('cpu'):
        x = array_ops.placeholder(np.float32, shape=[10, 200])
        y1 = array_ops.placeholder(np.int32, shape=[10])
        y2 = array_ops.placeholder(np.int32, shape=[10])
        report = gen_ipu_ops.ipu_event_trace()

      with ops.device("/device:IPU:0"):
        r = xla.compile(network, inputs=[x, y1, y2])

      tu.configure_ipu_system()

      sess.run(variables.global_variables_initializer())
      sess.run(report)
      out = sess.run(r, {
          x: np.ones(x.shape),
          y1: np.ones(y1.shape),
          y2: np.ones(y2.shape),
      })
      self.assertAllClose(out, [-4000.0])

      result = sess.run(report)
      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'Copy_',
          'ExpandDims/input/fusion*/multiUpdateAdd',
          'vs/Gather*/gather.*/multiSlice',
          'vs/add/add*/AddTo',
          'vs/Sum/reduce*/Reduce',
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testUnsortedSegmentSumVariableLR(self):
    with self.session() as sess:

      def network(x, y1, y2, lr):
        with variable_scope.variable_scope("vs", use_resource=True):
          w1 = variable_scope.get_variable(
              "w1",
              shape=[10, 200],
              dtype=np.float32,
              initializer=init_ops.constant_initializer(1))
          g1 = array_ops.gather(w1, y1)
          g2 = array_ops.gather(w1, y2)

          a = math_ops.reduce_sum(g1 + g2)

        optimizer = gradient_descent.GradientDescentOptimizer(lr)
        grads = [a]
        grads = [
            gradients_impl.gradients(g, variables.trainable_variables())[0]
            for g in grads
        ]
        grads = [array_ops.expand_dims(g, 0) for g in grads]
        grad = array_ops.concat(grads, axis=0)
        grad = math_ops.reduce_mean(grad, 0)
        train = optimizer.apply_gradients([(grad, w1)])
        return a, train

      with ops.device('cpu'):
        x = array_ops.placeholder(np.float32, shape=[10, 200])
        y1 = array_ops.placeholder(np.int32, shape=[10])
        y2 = array_ops.placeholder(np.int32, shape=[10])
        lr = array_ops.placeholder(np.float32, shape=[])
        report = gen_ipu_ops.ipu_event_trace()

      with ops.device("/device:IPU:0"):
        r = xla.compile(network, inputs=[x, y1, y2, lr])

      tu.configure_ipu_system()

      sess.run(variables.global_variables_initializer())
      sess.run(report)
      out = sess.run(
          r, {
              x: np.ones(x.shape),
              y1: np.ones(y1.shape),
              y2: np.ones(y2.shape),
              lr: 0.1,
          })
      self.assertAllClose(out, [-4000.0])

      result = sess.run(report)
      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = [
          '__seed*',
          'ExpandDims/input/fusion*/multiUpdateAdd',
          'ExpandDims/input/fusion*/negate_scale/Op/Negate',
          'vs/Gather*/gather.*/multiSlice',
          'vs/add/add*/AddTo',
          'vs/Sum/reduce*/Reduce',
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testScatterWithReshape(self):
    with self.session() as sess:

      def network(x, y1, y2, lr):
        with variable_scope.variable_scope("vs", use_resource=True):
          w = variable_scope.get_variable(
              "w",
              shape=[200, 10],
              dtype=np.float32,
              initializer=init_ops.constant_initializer(1))
          y = w * 2
          y = array_ops.reshape(y, [10, 200])
          g1 = nn.embedding_lookup(y, y1)
          g2 = nn.embedding_lookup(y, y1)
          g = array_ops.concat([g1, g2], axis=1)

          loss = math_ops.reduce_mean(g)

        optimizer = gradient_descent.GradientDescentOptimizer(lr)
        train = optimizer.minimize(loss)
        return loss, train

      with ops.device('cpu'):
        x = array_ops.placeholder(np.float32, shape=[1, 100, 100, 2])
        y1 = array_ops.placeholder(np.int32, shape=[10])
        y2 = array_ops.placeholder(np.int32, shape=[10])
        lr = array_ops.placeholder(np.float32, shape=[])
        report = gen_ipu_ops.ipu_event_trace()

      with ops.device("/device:IPU:0"):
        r = xla.compile(network, inputs=[x, y1, y2, lr])

      tu.configure_ipu_system()

      sess.run(variables.global_variables_initializer())
      sess.run(report)
      out = sess.run(
          r, {
              x: np.ones(x.shape),
              y1: np.ones(y1.shape),
              y2: np.ones(y2.shape),
              lr: 0.1,
          })
      self.assertAllClose(out, [2.0])

      result = sess.run(report)
      s = tu.extract_all_strings_from_event_trace(result)
      cs_list = tu.get_compute_sets_from_report(s)
      ok = [
          '__seed*',
          'GradientDescent/update_vs/w/ResourceApplyGradientDescent/fusion*/multiUpdateAdd',
          'GradientDescent/update_vs/w/ResourceApplyGradientDescent/fusion*/negate_scale/Op/Negate',
          'vs/mul/fusion*/Op/Multiply',
          'vs/embedding_lookup/gather.*/multiSlice',
          '/reduce*/Reduce*/Reduce',
          'vs/Mean/add/Op/Add',
          'vs/Mean/multiply/Op/Multiply',
          'Copy_*',
      ]
      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))


if __name__ == "__main__":
  os.environ['TF_XLA_FLAGS'] = (
      '--tf_xla_min_cluster_size=1 ' + os.environ.get('TF_XLA_FLAGS', ''))
  googletest.main()
