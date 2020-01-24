# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for IPU LSTM layers."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.platform import test
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import rnn
from tensorflow.python.ops import rnn_cell

from tensorflow.python import ipu

from tensorflow.python.ipu.function import function as ipu_function

# Test hyperparameters.
batch_size = 1
num_input = 3
timesteps = 3
num_hidden = 3
dataType = np.float32

# The keras upstream returns a slightly different result to both the TF rnn_cell.LSTMCell and the popNN custom layer.
# def _tfLSTM_Keras(x, h, c):
#     layer = keras.layers.LSTM(num_hidden, kernel_initializer=init_ops.constant_initializer(dtype=dataType),
#                                           recurrent_initializer=None,
#                                           bias_initializer=init_ops.zeros_initializer(dtype=dataType),
#                                           return_state=True, return_sequences=True, time_major=True)

#     @ipu_function
#     def impl(layer, x, h, c):
#         layer.build(x.shape)
#         return layer(x, initial_state=[c, h])

#     return impl(layer, x, h, c)


def _tfLSTM(x, h, c):
  lstm_cell = rnn_cell.LSTMCell(
      num_hidden,
      name='basic_lstm_cell',
      forget_bias=0.,
      initializer=init_ops.random_uniform_initializer(seed=42, dtype=dataType))
  state = rnn_cell.LSTMStateTuple(c, h)

  @ipu_function
  def impl(
      cell,
      x,
      state,
  ):
    return rnn.dynamic_rnn(cell,
                           x,
                           dtype=dataType,
                           initial_state=state,
                           time_major=True)

  return impl(lstm_cell, x, state)


def _new_kerasLSTM(x, h, c):
  layer = ipu.layers.PopnnLSTM(
      num_hidden,
      dtype=dataType,
      weights_initializer=init_ops.random_uniform_initializer(seed=42,
                                                              dtype=dataType),
      recurrent_weight_initializer=None,
      bias_initializer=init_ops.zeros_initializer(dtype=dataType))
  layer.build(x.shape)

  state = rnn_cell.LSTMStateTuple(c, h)
  return layer(inputs=x, initial_state=state)


def _tfGRU(x, initial_state):
  gru_cell = rnn_cell.GRUCell(
      num_hidden,
      name='gru_cell',
      kernel_initializer=init_ops.zeros_initializer(dtype=dataType),
      bias_initializer=init_ops.constant_initializer(2.0, dtype=dataType))

  @ipu_function
  def impl(x, initial_state, cell):
    return rnn.dynamic_rnn(cell,
                           x,
                           dtype=dataType,
                           initial_state=initial_state,
                           time_major=True)

  return impl(x, initial_state, gru_cell)


def _new_kerasGRU(x, initial_state):
  layer = ipu.layers.PopnnGRU(
      num_hidden,
      dtype=dataType,
      weights_initializer=init_ops.zeros_initializer(dtype=dataType),
      bias_initializer=init_ops.constant_initializer(2.0, dtype=dataType))
  layer.build(x.shape)
  return layer(x, initial_state=initial_state)


class IpuLstmTest(test.TestCase):
  def test_lstm(self):
    np.random.seed(42)
    x = np.random.rand(timesteps, batch_size, num_input).astype(dataType)

    np.random.seed(42)
    keras_result = _new_kerasLSTM(
        x, array_ops.ones((batch_size, num_hidden), dtype=dataType),
        array_ops.ones((batch_size, num_hidden), dtype=dataType))

    np.random.seed(42)
    result_tf = _tfLSTM(
        x, array_ops.ones((batch_size, num_hidden), dtype=dataType),
        array_ops.ones((batch_size, num_hidden), dtype=dataType))

    # Check they are the same.
    self.assertAllClose(keras_result[0], result_tf[0], rtol=0.01)

    self.assertAllClose(keras_result[1], result_tf[1], rtol=0.2)


class IpuGruTest(test.TestCase):
  def test_gru(self):
    np.random.seed(42)
    x = np.random.rand(timesteps, batch_size, num_input).astype(dataType)
    # Get the "normal" tensorflow result
    keras_result = _new_kerasGRU(
        x, np.ones((batch_size, num_hidden), dtype=dataType))
    result_tf = _tfGRU(x, np.ones((batch_size, num_hidden), dtype=dataType))
    # Check they are the same.
    self.assertAllClose(keras_result, result_tf)


if __name__ == '__main__':
  test.main()
