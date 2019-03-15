# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
# =============================================================================
"""Contains class for creating output outfeeds into TF graphs targeting the IPU."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.compiler.plugin.poplar.ops import gen_pop_datastream_ops
from tensorflow.python.framework import ops
from copy import deepcopy


class IPUOutfeedQueue:
    """Generates and adds outfeed enqueue/dequeue operations to the graph.

    The queue has two modes of operation - outfeed all or outfeed last.
    In outfeed all mode every element that is enqueued will be stored
    for a subsequent dequeue. All of the enqueued elements will be returned
    when the dequeue operation is run.

    In outfeed last mode only the last enqueued element is stored. The dequeue
    operation will in this case return a single element.
    """

    def __init__(self, outfeed_all=True, device_ordinal=0):
      """Creates an IPUOutfeedQueue object.

        Args:
            outfeed_all: a bool value indicating whether all enqueued
            elements should be stored or only the last one.
            device_ordinal: ordinal of the device on which this queue will be
            used.

        Raises:
          ValueError: if the types or values are incorrect
      """
      if type(outfeed_all) is not bool:
          raise ValueError("Expcted value True or False for outfeed_all")

      if type(device_ordinal) is not int:
          raise ValueError('Device ordinal must be an integer')

      if device_ordinal < 0:
          raise ValueError('Device ordinal must be >= 0')

      self._outfeed_all = outfeed_all
      self._device_ordinal = device_ordinal
      self._outfeed_mode = 'all' if outfeed_all else 'get_last'

      self._enqueued = False
      self._dtypes = []
      self._shapes = []
      self._device_str = '/device:IPU:{}'.format(str(device_ordinal))

    def _is_iterable(self, x):
      return isinstance(x, list) or isinstance(x, tuple)

    def enqueue(self, tensors):
      """Enqueue a list or tuple of tensors for being outfed from the IPU
         graph. This operation is placed on the IPU device. If you are using
         the enqueue operation within a `contrib.ipu.training_loop` body then
         the resulting operation needs to be added to the return value tuple
         of the body otherwise the operation is not executed.
         An example demonstrating this:
        ```
        outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

        def body(v):
          outfeed = outfeed_queue.enqueue([v])
          v = v + 1
          return (v, outfeed)

        def my_net(v):
          r = loops.repeat(20, body, (v))
          return r

        with ipu.ops.ipu_scope("/device:IPU:0"):
          res = ipu_compiler.compile(my_net, inputs=[v])

        ...
        ...

        outfeed = outfeed_queue.dequeue()
        with tf.Session() as sess:
          result = sess.run(res, {v:np.ones([4, 4], np.float32)})
          outfed = sess.run(outfeed)
        ```
      """
      if not self._is_iterable(tensors):
          raise ValueError("Expected tensors to be of type list or tuple")

      for t in tensors:
          if self._is_iterable(t):
              raise ValueError("Nested inputs are not supported")

      if len(self._shapes) == 0:
          for t in tensors:
              self._shapes.append(t.get_shape())
              self._dtypes.append(t.dtype)

      with ops.device(self._device_str):
          outfeed_op = gen_pop_datastream_ops.pop_datastream_outfeed_enqueue(
              tensors, outfeed_mode=self._outfeed_mode)

      self._enqueued = True
      return outfeed_op

    @property
    def enqueued(self):
        return self._enqueued

    def dequeue(self, shapes=[], dtypes=[]):
      """Generate host side operation to dequeue the outfed values. The
      operation generated by this function will block if called prior
      to any enqueues. The `shapes` and `types` argument is optional depending
      on whether or not there is an enqueue operation in the IPU graph prior
      to calling dequeue. If there is an enqueue operation from before then the
      shapes and types of the enqueued tensors will be used.

      If the enqueued tensors are tuples and this class was initialized with
      `outfeed_all=True` then the return value is a list of tensors in which
      each element of the list corresponds to all of the outfed values for
      the same index in the tuple.
      For example if the body in a `contrib.ipu.training_loop` looks something like:
      ```
      def body(v, u):
        v = v + 1
        u = u + 2
        outfeed = outfeed_queue.enqueue([v, u])
        return (v, u, outfeed)


      with ops.device('cpu'):
        v = tf.placeholder(np.float32, [4, 4])
        u = tf.placeholder(np.float32, [2, 2])

      def my_net(v, u):
        r = loops.repeat(20, body, (v, u))
        return r


      with ipu.ops.ipu_scope("/device:IPU:0"):
        res = ipu_compiler.compile(my_net, inputs=[v, u])

      outfeed = outfeed_queue.dequeue()

      with tf.Session() as sess:
        result = sess.run(res, {v:np.ones([4, 4], np.float32)})
        outfed = sess.run(outfeed)
      ```

      Then outfed will be a list of two tensors where the first tensor is of
      shape (20, 4, 4) and the second tensor (20, 2, 2)

      """
      if not self._is_iterable(shapes) or not self._is_iterable(dtypes):
          raise ValueError(
              'shapes and dtypes must be either tuple or list type')

      if len(shapes) != len(dtypes):
          raise ValueError("Number of shapes and datatypes must be the same")

      if not self._enqueued and len(shapes) == 0:
          raise ValueError(
              """An enqueue op has not been added to the graph and
              no shapes or types have been provided""")

      if (len(shapes) > 0 or len(dtypes) > 0) and not self._enqueued:
          for (s, t) in zip(shapes, dtypes):
              shape_is_nested = self._is_iterable(s)
              dtype_is_nested = self._is_iterable(t)
              if shape_is_nested or dtype_is_nested:
                  raise ValueError("Nested inputs are not supported")

          self._shapes = shapes
          self._dtypes = dtypes

      # None shape in beginning of list indicates that an unknown number of
      # outfed elements will be returned

      outfeed_shapes = deepcopy(self._shapes)
      if self._outfeed_all and self._shapes[0] is not None:
          outfeed_shapes.insert(0, None)
      with ops.device('cpu'):
          outfeed_dequeue = \
          gen_pop_datastream_ops.pop_datastream_outfeed_dequeue(
              output_types=self._dtypes, output_shapes=outfeed_shapes)

      return outfeed_dequeue
