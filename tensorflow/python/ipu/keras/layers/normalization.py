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
""" Keras layers exposing Graphcore IPU specific normalization functions """
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.framework import dtypes
from tensorflow.python.keras.engine.base_layer import Layer
from tensorflow.python.ops import array_ops

from tensorflow.compiler.plugin.poplar.ops import gen_popnn_ops


# We implement all three algorithms through a common generic group norm algorithm.
class GroupNorm(Layer):
  def __init__(self,
               dtype=dtypes.float32,
               groups=2,
               channels_axis=-1,
               reduction_axes=(-3, -2),
               center=True,
               scale=True,
               epsilon=1e-6,
               beta_initializer=None,
               gamma_initializer=None,
               name=None):
    super(GroupNorm, self).__init__(dtype=dtype, name=name)

    self.groups = groups
    self.channels_axis = channels_axis
    self.reduction_axes = reduction_axes
    self.center = center
    self.scale = scale
    self.epsilon = epsilon

    self.beta_initializer = beta_initializer
    self.gamma_initializer = gamma_initializer

    self.data_format = ""
    self.channels = 1

  def build(self, input_shape):

    if self.built:
      return

    if input_shape is None:
      raise ValueError('Input has undefined rank.')
    if self.channels_axis > (len(input_shape) - 1):
      raise ValueError('Axis is out of bounds.')

    # Standardize the channels_axis to be positive and identify # of channels.
    if self.channels_axis < 0:
      self.channels_axis = len(input_shape) + self.channels_axis
    self.channels = input_shape[self.channels_axis]

    # Standardize the reduction_axes to be positive.
    self.reduction_axes = list(self.reduction_axes)
    for i, _ in enumerate(self.reduction_axes):
      if self.reduction_axes[i] < 0:
        self.reduction_axes[i] += len(input_shape)

    for a in self.reduction_axes:
      if a > len(input_shape):
        raise ValueError('Axis is out of bounds.')
      if input_shape[a] is None:
        raise ValueError('Input has undefined dimensions.')
      if self.channels_axis == a:
        raise ValueError('reduction_axis must be mutually exclusive '
                         'with channels_axis')
    if self.groups > self.channels:
      raise ValueError('Invalid groups %d for %d channels.' %
                       (self.groups, self.channels))
    if self.channels % self.groups != 0:
      raise ValueError('%d channels is not commensurate with %d groups.' %
                       (self.channels, self.groups))

    # Check which format the data is in.
    if self.channels_axis == 1:
      self.data_format = "NCHW"
    elif self.channels_axis == len(input_shape) - 1:
      self.data_format = "NHWC"
    else:
      raise ValueError('Unsupported data format, group norm only supports NCHW'
                       '(channel axis 1) and NHWC (channel axis -1).')
    params_shape = [self.channels]

    if self.center:
      self.beta = self.add_weight("beta",
                                  dtype=self.dtype,
                                  initializer=self.beta_initializer,
                                  shape=params_shape)

    if self.scale:
      self.gamma = self.add_weight("gamma",
                                   dtype=self.dtype,
                                   initializer=self.gamma_initializer,
                                   shape=params_shape)

    self.built = True

  # pylint: disable=arguments-differ
  def call(self, inputs, training=True):

    params_shape = [self.channels]

    # TensorFlow doesn't like constants being created in the build func.
    if not self.center:
      self.beta = array_ops.constant(0.0, dtype=self.dtype, shape=params_shape)

    if not self.scale:
      self.gamma = array_ops.constant(1.0,
                                      dtype=self.dtype,
                                      shape=params_shape)

    if training:
      outputs, _, _ = gen_popnn_ops.popnn_group_norm_training(
          inputs=inputs,
          gamma=self.gamma,
          beta=self.beta,
          data_format=self.data_format,
          epsilon=self.epsilon,
          num_groups=self.groups)

    else:
      # Calculate the moments.
      mean, inv_std_dev = gen_popnn_ops.popnn_group_norm_statistics(
          inputs=inputs,
          data_format=self.data_format,
          epsilon=self.epsilon,
          num_groups=self.groups)

      outputs = gen_popnn_ops.popnn_group_norm_inference(
          inputs=inputs,
          gamma=self.gamma,
          beta=self.beta,
          mean=mean,
          inv_std_dev=inv_std_dev,
          data_format=self.data_format,
          epsilon=self.epsilon,
          num_groups=self.groups)
    return outputs


class InstanceNorm(GroupNorm):
  def __init__(self,
               dtype=dtypes.float32,
               channels_axis=-1,
               reduction_axes=(-3, -2),
               center=True,
               scale=True,
               epsilon=1e-6,
               beta_initializer=None,
               gamma_initializer=None,
               name=None):
    super(InstanceNorm, self).__init__(dtype=dtype,
                                       groups=1,
                                       channels_axis=channels_axis,
                                       reduction_axes=reduction_axes,
                                       center=center,
                                       scale=scale,
                                       epsilon=epsilon,
                                       beta_initializer=beta_initializer,
                                       gamma_initializer=gamma_initializer,
                                       name=name)

  # pylint: disable=useless-super-delegation
  def build(self, input_shape):
    super(InstanceNorm, self).build(input_shape)

  # pylint: disable=useless-super-delegation
  def call(self, inputs, training=True):
    return super(InstanceNorm, self).call(inputs, training)


class LayerNorm(GroupNorm):
  def __init__(self,
               dtype=dtypes.float32,
               channels_axis=-1,
               reduction_axes=(-3, -2),
               center=True,
               scale=True,
               epsilon=1e-6,
               beta_initializer=None,
               gamma_initializer=None,
               name=None):
    super(LayerNorm, self).__init__(
        dtype=dtype,
        # We set this in the build function, once we know what the shape is.
        groups=0,
        channels_axis=channels_axis,
        reduction_axes=reduction_axes,
        center=center,
        scale=scale,
        epsilon=epsilon,
        beta_initializer=beta_initializer,
        gamma_initializer=gamma_initializer,
        name=name)

  def build(self, input_shape):
    # Change the groups based on the input shape.
    self.groups = input_shape[self.channels_axis]
    super(LayerNorm, self).build(input_shape)

  # pylint: disable=useless-super-delegation
  def call(self, inputs, training=True):
    return super(LayerNorm, self).call(inputs, training)
