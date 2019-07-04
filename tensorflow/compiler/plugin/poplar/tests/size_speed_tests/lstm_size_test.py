from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import numpy as np

from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.python import ipu
from tensorflow.python.platform import googletest
from tensorflow.python.framework import test_util
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import rnn
from tensorflow.python.ops import rnn_cell
from tensorflow.python.ops import variables
from tensorflow.python.client import session as sl

dataType = np.float16

batch_size = 1
num_input = 28
timesteps = 5
num_hidden = 512


def _PopnnLSTM(x, h, c):
  lstm_cell = ipu.ops.rnn_ops.PopnnLSTM(
      num_hidden,
      dtype=dataType,
      weights_initializer=init_ops.zeros_initializer(dtype=dataType),
      bias_initializer=init_ops.zeros_initializer(dtype=dataType))
  state = rnn_cell.LSTMStateTuple(c, h)
  return lstm_cell(x, initial_state=state, training=False)


def _tfLSTM(x, h, c):
  lstm_cell = rnn_cell.LSTMCell(
      num_hidden,
      name='basic_lstm_cell',
      forget_bias=0.,
      initializer=init_ops.zeros_initializer(dtype=dataType))
  state = rnn_cell.LSTMStateTuple(c, h)
  return rnn.dynamic_rnn(
      lstm_cell, x, dtype=dataType, initial_state=state, time_major=True)


def RunLayer(layer_func, x):
  with ops.device('cpu'):
    px = array_ops.placeholder(dataType, shape=x.shape)
    ph = array_ops.placeholder(dataType, shape=[batch_size, num_hidden])
    pc = array_ops.placeholder(dataType, shape=[batch_size, num_hidden])
    report = gen_ipu_ops.ipu_event_trace()
  with ipu.scopes.ipu_scope("/device:IPU:0"):
    r = ipu.ipu_compiler.compile(layer_func, inputs=[px, ph, pc])

  opts = ipu.utils.create_ipu_config(profiling=True)
  ipu.utils.configure_ipu_system(opts)

  with sl.Session() as sess:
    sess.run(variables.global_variables_initializer())
    out = sess.run(report)
    result = sess.run(r, {px: x, ph: np.ones(ph.shape), pc: np.ones(pc.shape)})
    out = sess.run(report)
    evts = ipu.utils.extract_all_events(out)
    size = ipu.utils.get_memory_size_from_events(evts)
  return (size, result)


class LstmSizeTest(test_util.TensorFlowTestCase):
  # Test which verifies that:
  # 1. Custom op uses less memory
  # 2. Custom op and Tf op return the same result
  def testCustomOpIsSmaller(self):
    np.random.seed(42)
    x = np.random.rand(timesteps, batch_size, num_input).astype(dataType)
    size_custom_op, result_custom_op = RunLayer(_PopnnLSTM, x)
    size_tf, result_tf = RunLayer(_tfLSTM, x)
    self.assertAllClose(result_custom_op, result_tf)
    self.assertTrue(size_custom_op < size_tf)


if __name__ == "__main__":
  os.environ['TF_XLA_FLAGS'] = (
      '--tf_xla_min_cluster_size=1 ' + os.environ.get('TF_XLA_FLAGS', ''))
  googletest.main()
