# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import test_util as tu

from tensorflow.contrib.ipu.python import autoshard
from tensorflow.contrib.ipu.python import ipu_compiler
from tensorflow.contrib.ipu.python import ipu_infeed_queue
from tensorflow.contrib.ipu.python import loops
from tensorflow.contrib.ipu.python import popnn_rnn
from tensorflow.contrib.ipu.python import sharded_optimizer as so
from tensorflow.contrib.ipu.python import sharding
from tensorflow.keras import layers
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops as nn
from tensorflow.python.platform import googletest
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.training import gradient_descent as gd

logging.set_verbosity(logging.DEBUG)

allowed_op_types = [
    'NoOp', 'Identity', 'XlaClusterOutput', 'Enter', 'Exit', 'Switch', 'Merge',
    'NextIteration', 'LoopCond', 'VarHandleOp', 'Const'
]


def get_single_while_op_body(g):
  outer_ops = g.get_operations()
  while_ops = list(filter(lambda x: x.type == 'While', outer_ops))
  assert (len(while_ops) == 1)
  body = g._get_function(while_ops[0].get_attr('body').name)
  return body.graph


class AutoshardTest(test_util.TensorFlowTestCase):
  def testSimpleXlaCompileInference(self):
    def my_model(inp):
      output = inp * inp
      return [output]

    with ops.device("cpu"):
      inp = array_ops.placeholder(np.float32, [], name="a")

    with ops.device("/device:IPU:0"):
      out = ipu_compiler.compile(my_model, inputs=[inp])

    autoshard.automatic_sharding(2, inp, out[0])

    op_list = ops.get_default_graph().get_operations()
    for o in op_list:
      if o.device == '/device:IPU:0' and o.type != 'NoOp':
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

  def testSimpleXlaCompileTraining(self):
    def my_model(inp, lab):

      x = inp
      y = lab

      x = layers.Conv2D(8, 3, padding='same', name="conv1", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv2", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv3", use_bias=False)(x)
      x = math_ops.reduce_max(x, axis=[1, 2])

      cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
          logits=x, labels=array_ops.stop_gradient(y))
      loss = math_ops.reduce_mean(cross_entropy)
      optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
      train = optim.minimize(cross_entropy)

      autoshard.automatic_sharding(2, inp, loss)

      return [loss, train]

    with ops.device("cpu"):
      inp = array_ops.placeholder(np.float32, [1, 12, 12, 4], name="data")
      lab = array_ops.placeholder(np.float32, [1, 8], name="labl")

    with ops.device("/device:IPU:0"):
      out = ipu_compiler.compile(my_model, inputs=[inp, lab])

    op_set = sharding.dependencies([out[0]])

    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

  def testSimpleTraining(self):
    def my_model(x, y):
      x = layers.Conv2D(8, 3, padding='same', name="conv1", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv2", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv3", use_bias=False)(x)
      x = math_ops.reduce_max(x, axis=[1, 2])

      cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
          logits=x, labels=array_ops.stop_gradient(y))
      loss = math_ops.reduce_mean(cross_entropy)
      optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
      train = optim.minimize(cross_entropy)
      return [loss, train]

    with ops.device("cpu"):
      inp = array_ops.placeholder(np.float32, [1, 12, 12, 4], name="data")
      lab = array_ops.placeholder(np.float32, [1, 8], name="labl")

    with ops.device("/device:IPU:0"):
      l, t = my_model(inp, lab)

    autoshard.automatic_sharding(2, inp, l)

    op_set = sharding.dependencies([l, t])

    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

  def testSimpleTrainingWithEdgeFilter(self):
    def my_model(x, y):
      x = layers.Conv2D(8, 3, padding='same', name="conv1", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv2", use_bias=False)(x)
      x = layers.Conv2D(8, 3, padding='same', name="conv3", use_bias=False)(x)
      x = math_ops.reduce_max(x, axis=[1, 2])

      cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
          logits=x, labels=array_ops.stop_gradient(y))
      loss = math_ops.reduce_mean(cross_entropy)
      optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
      train = optim.minimize(cross_entropy)
      return [loss, train]

    with ops.device("cpu"):
      inp = array_ops.placeholder(np.float32, [1, 12, 12, 4], name="data")
      lab = array_ops.placeholder(np.float32, [1, 8], name="labl")

    with ops.device("/device:IPU:0"):
      l, t = my_model(inp, lab)

    filt = lambda e: not (e[0] != 'conv2/Conv2D' and e[1] != 'conv3/Conv2D')

    autoshard.automatic_sharding(2, inp, l, edge_filter=filt)

    op_set = sharding.dependencies([l, t])

    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

  def testSimpleXlaCompileTrainingInLoop(self):
    dataset = tu.create_dual_increasing_dataset(3)

    infeed_queue = ipu_infeed_queue.IPUInfeedQueue(dataset)

    def my_net():
      def my_model(loss, x, y):
        with ops.device("/device:IPU:0"):
          inp = x

          x = layers.Conv2D(
              8, 3, padding='same', name="conv1", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv2", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv3", use_bias=False)(x)
          x = math_ops.reduce_max(x, axis=[1, 2])

          cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
              logits=x, labels=array_ops.stop_gradient(y))
          loss = math_ops.reduce_mean(cross_entropy)

          optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
          train = optim.minimize(cross_entropy)

          autoshard.automatic_sharding(2, inp, loss)

          return [loss, train]

      loss = 0.0
      return loops.repeat(
          10, my_model, [loss], infeed_queue, use_while_v1=False)

    ipu_compiler.compile(my_net, inputs=[])

    body = get_single_while_op_body(ops.get_default_graph())
    op_set = body.get_operations()
    op_types = set()
    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        op_types.add(o.type)
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

    self.assertTrue(len(op_types) > 10)
    self.assertTrue('Conv2D' in op_types)
    self.assertTrue('Conv2DBackpropInput' in op_types)
    self.assertTrue('Conv2DBackpropFilter' in op_types)
    self.assertTrue('ResourceApplyGradientDescent' in op_types)

  def testPopnnLstmXlaCompileTrainingInLoop(self):
    dataset = tu.create_dual_increasing_dataset(
        3, data_shape=[16, 2, 8], label_shape=[16, 2, 256])

    infeed_queue = ipu_infeed_queue.IPUInfeedQueue(dataset)

    def my_net():
      def my_model(loss, x, y):
        with ops.device("/device:IPU:0"):
          inp = x

          lstm_cell = popnn_rnn.PopnnLSTM(256, dtype=dtypes.float32)
          x, _ = lstm_cell(x, training=True)

          cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
              logits=x, labels=array_ops.stop_gradient(y))
          loss = math_ops.reduce_mean(cross_entropy)

          optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
          train = optim.minimize(cross_entropy)

          autoshard.automatic_sharding(2, inp, loss)

          return [loss, train]

      loss = 0.0
      return loops.repeat(
          10, my_model, [loss], infeed_queue, use_while_v1=False)

    ipu_compiler.compile(my_net, inputs=[])

    body = get_single_while_op_body(ops.get_default_graph())
    op_set = body.get_operations()
    op_types = set()

    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        op_types.add(o.type)
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

    self.assertTrue(len(op_types) > 10)
    self.assertTrue('PopnnLstmLayer' in op_types)
    self.assertTrue('PopnnLstmLayerBackprop' in op_types)
    self.assertTrue('LogSoftmax' in op_types)
    self.assertTrue('SoftmaxCrossEntropyWithLogits' in op_types)
    self.assertTrue('ResourceApplyGradientDescent' in op_types)

  def testSimpleXlaCompileTrainingInLoopWithParam(self):
    dataset = tu.create_dual_increasing_dataset(3)

    infeed_queue = ipu_infeed_queue.IPUInfeedQueue(dataset)

    def my_net(lr):
      def my_model(loss, x, y):
        with ops.device("/device:IPU:0"):
          inp = x

          x = layers.Conv2D(
              8, 3, padding='same', name="conv1", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv2", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv3", use_bias=False)(x)
          x = math_ops.reduce_max(x, axis=[1, 2])

          cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
              logits=x, labels=array_ops.stop_gradient(y))
          loss = math_ops.reduce_mean(cross_entropy)

          optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(lr))
          train = optim.minimize(cross_entropy)

          autoshard.automatic_sharding(2, inp, loss)

          return [loss, train]

      loss = 0.0
      return loops.repeat(
          10, my_model, [loss], infeed_queue, use_while_v1=False)

    lr = array_ops.placeholder(dtypes.float32, [])
    ipu_compiler.compile(my_net, inputs=[lr])

    body = get_single_while_op_body(ops.get_default_graph())
    op_set = body.get_operations()
    op_types = set()
    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        op_types.add(o.type)
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

    self.assertTrue(len(op_types) > 10)
    self.assertTrue('Conv2D' in op_types)
    self.assertTrue('Conv2DBackpropInput' in op_types)
    self.assertTrue('Conv2DBackpropFilter' in op_types)
    self.assertTrue('ResourceApplyGradientDescent' in op_types)

    # TODO re-enable this when while loops can be compiled correctly : T7502
    # def testTfLstmXlaCompileTrainingInLoop(self):
    #   dataset = tu.create_dual_increasing_dataset(3, data_shape=[16, 2, 8],
    #                                          label_shape=[16, 2, 256])
    #
    #   infeed_queue = ipu_infeed_queue.IPUInfeedQueue(dataset)
    #
    #   def my_net():
    #     def my_model(loss):
    #       with ops.device("/device:IPU:0"):
    #         inp, lab = infeed_queue.get_next()
    #         x = inp
    #         y = lab
    #
    #         lstm_cell = rnn_cell.LSTMCell(256)
    #         x, _ = rnn.dynamic_rnn(cell=lstm_cell, inputs=x,
    #                                dtype=dtypes.float32, time_major=True)
    #
    #         cross_entropy = nn.softmax_cross_entropy_with_logits(logits=x, labels=y)
    #         loss = math_ops.reduce_mean(cross_entropy)
    #
    #         optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
    #         train = optim.minimize(cross_entropy)
    #
    #         autoshard.automatic_sharding(2, inp, loss, [])
    #
    #         return [loss, train]
    #
    #     loss = 0.0
    #     return loops.repeat(10, my_model, [loss], infeed_queue)
    #
    #   ipu_compiler.compile(my_net, inputs=[])
    #
    #   body = get_single_while_op_body(ops.get_default_graph())
    #   op_set = body.get_operations()
    #   op_types = set()
    #
    #   for o in op_set:
    #     if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
    #       op_types.add(o.type)
    #       self.assertTrue(o.get_attr('_XlaSharding') is not None)
    #
    #   self.assertTrue(len(op_types) > 10)
    #   self.assertTrue('While' in op_types)
    #   self.assertTrue('LogSoftmax' in op_types)
    #   self.assertTrue('SoftmaxCrossEntropyWithLogits' in op_types)
    #   self.assertTrue('ResourceApplyGradientDescent' in op_types)

  def testSimpleXlaCompileTrainingInLoopV1WithEarlySharding(self):

    dataset = tu.create_dual_increasing_dataset(3)

    infeed_queue = ipu_infeed_queue.IPUInfeedQueue(dataset)

    def my_net():
      def my_model(loss, x, y):
        with ops.device("/device:IPU:0"):
          inp = x

          x = layers.Conv2D(
              8, 3, padding='same', name="conv1", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv2", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv3", use_bias=False)(x)
          x = math_ops.reduce_max(x, axis=[1, 2])

          cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
              logits=x, labels=array_ops.stop_gradient(y))
          loss = math_ops.reduce_mean(cross_entropy)

          optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
          train = optim.minimize(cross_entropy)

          autoshard.automatic_sharding(2, inp, loss)

          return [loss, train]

      loss = 0.0
      return loops.repeat(
          10, my_model, [loss], infeed_queue, use_while_v1=True)

    ipu_compiler.compile(my_net, inputs=[])

    op_set = ops.get_default_graph().get_operations()
    op_types = set()

    for o in op_set:
      if o.device == '/device:IPU:0' and o.type not in allowed_op_types:
        op_types.add(o.type)
        self.assertTrue(o.get_attr('_XlaSharding') is not None)

    self.assertTrue(len(op_types) > 10)
    self.assertTrue('Conv2D' in op_types)
    self.assertTrue('Conv2DBackpropInput' in op_types)
    self.assertTrue('Conv2DBackpropFilter' in op_types)
    self.assertTrue('ResourceApplyGradientDescent' in op_types)

  def testMarkOpsWithAutoshardingContext(self):

    with ops.device("/device:IPU:0"):

      with autoshard.ipu_autoshard():
        x = array_ops.placeholder(dtypes.float32, shape=[1, 32, 32, 4])
        y = array_ops.placeholder(dtypes.float32, shape=[1, 8])

        inp = x

        with ops.name_scope('gradients'):

          x = layers.Conv2D(
              8, 3, padding='same', name="conv1", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv2", use_bias=False)(x)
          x = layers.Conv2D(
              8, 3, padding='same', name="conv3", use_bias=False)(x)
          x = math_ops.reduce_max(x, axis=[1, 2])

        cross_entropy = nn.softmax_cross_entropy_with_logits_v2(
            logits=x, labels=array_ops.stop_gradient(y))
        loss = math_ops.reduce_mean(cross_entropy)

      optim = so.ShardedOptimizer(gd.GradientDescentOptimizer(0.01))
      optim.minimize(loss)

      autoshard.automatic_sharding(2, inp, loss)

    to_autoshard = ops.get_default_graph().get_collection(
        sharding._IPU_AUTOSHARD)

    fwd_ops = []
    bwd_ops = []

    all_ops = ops.get_default_graph().get_operations()
    for o in all_ops:
      if o in to_autoshard:
        fwd_ops.append(o)
      else:
        bwd_ops.append(o)

    self.assertTrue(len(fwd_ops) > 10)
    self.assertTrue(len(bwd_ops) > 10)

    self.assertEqual(len([o for o in fwd_ops if 'Conv2D' == o.type]), 3)


if __name__ == "__main__":
  googletest.main()
