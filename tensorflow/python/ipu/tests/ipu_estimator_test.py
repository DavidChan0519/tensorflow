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
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import glob
import six
import numpy as np

from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.compiler.plugin.poplar.tests import test_utils as tu
from tensorflow.keras import layers
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.estimator import model_fn as model_fn_lib
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ipu import ipu_estimator
from tensorflow.python.ipu import ipu_run_config
from tensorflow.python.ipu import utils as ipu_utils
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import metrics_impl
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops.losses import losses
from tensorflow.python.platform import googletest
from tensorflow.python.summary import summary_iterator
from tensorflow.python.training import gradient_descent
from tensorflow.python.training import session_run_hook


def _dummy_model_fn(features, labels, params):
  _, _, _ = features, labels, params


def _create_regression_dataset(num_samples, num_features):
  np.random.seed(1234)
  target_weights = np.random.rand(num_features, 1).astype(np.float32)
  X = np.random.rand(num_samples, num_features).astype(np.float32)
  y = np.matmul(X, target_weights)
  return X, y


class _SessionRunCounter(session_run_hook.SessionRunHook):
  def __init__(self):
    self.num_session_runs = 0

  def after_run(self, run_context, run_values):
    self.num_session_runs += 1


class IPUEstimatorTest(test_util.TensorFlowTestCase):
  def testConstructor(self):
    config = ipu_run_config.RunConfig()
    estimator = ipu_estimator.IPUEstimator(model_fn=_dummy_model_fn,
                                           model_dir="bla",
                                           config=config)
    self.assertTrue(estimator.model_dir == "bla")
    self.assertTrue(isinstance(estimator.config, ipu_run_config.RunConfig))

  def testTrain(self):
    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      with variable_scope.variable_scope("vs", use_resource=True):
        predictions = layers.Dense(units=1)(features)

      loss = losses.mean_squared_error(labels=labels, predictions=predictions)
      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train_op = optimizer.minimize(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = dataset_ops.Dataset.from_tensor_slices(
          _create_regression_dataset(num_samples=1000, num_features=5))
      dataset = dataset.batch(batch_size=2, drop_remainder=True).repeat()
      return dataset

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=2),
        log_step_count_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    session_run_counter = _SessionRunCounter()

    estimator.train(input_fn=my_input_fn, steps=4, hooks=[session_run_counter])

    self.assertEquals(2, session_run_counter.num_session_runs)
    self.assertEquals(4, estimator.get_variable_value("global_step"))

    # Calling it again should work
    estimator.train(input_fn=my_input_fn, steps=4, hooks=[session_run_counter])

    self.assertEquals(4, session_run_counter.num_session_runs)
    self.assertEquals(8, estimator.get_variable_value("global_step"))

    # The number of steps is rounded up to the
    # next multiple of `iterations_per_loop`
    estimator.train(input_fn=my_input_fn,
                    max_steps=9,
                    hooks=[session_run_counter])

    self.assertEquals(5, session_run_counter.num_session_runs)
    self.assertEquals(10, estimator.get_variable_value("global_step"))

  def testPassingParams(self):
    exepected_params = {"my_param": 42}

    def my_input_fn(mode, params):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)
      self.assertTrue("my_param" in params)
      self.assertTrue(params["my_param"] == 42)
      dataset = tu.create_dual_increasing_dataset(10,
                                                  data_shape=[1],
                                                  label_shape=[1])
      dataset = dataset.batch(batch_size=1, drop_remainder=True)
      return dataset

    def my_model_fn(features, labels, mode, params):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)
      self.assertTrue("my_param" in params)
      self.assertTrue(params["my_param"] == 42)
      loss = math_ops.reduce_sum(features + labels, name="loss")
      train_op = array_ops.identity(loss)
      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig(),
                                           params=exepected_params)

    estimator.train(input_fn=my_input_fn, steps=1)
    self.assertTrue("my_param" in estimator.params)
    self.assertTrue(estimator.params["my_param"] == 42)

  def testHostCallOneArgument(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensor_slices(([1., 1.], [0., 1.]))

    def my_host_fn(loss):
      loss_sum = variable_scope.get_variable(name="loss_sum", initializer=0.0)
      self.assertEqual("/device:CPU:*", loss_sum.device)
      return loss_sum.assign_add(loss)

    def my_model_fn(features, labels, mode):
      loss = features + labels
      self.assertEqual("/device:IPU:0", loss.device)
      train_op = array_ops.identity(loss)
      host_call = (my_host_fn, [loss])
      return ipu_estimator.IPUEstimatorSpec(mode=mode,
                                            loss=loss,
                                            train_op=train_op,
                                            host_call=host_call)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    estimator.train(input_fn=my_input_fn, steps=2)
    self.assertEqual(3., estimator.get_variable_value("loss_sum"))

  def testHostCallZeroArguments(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensor_slices(([1., 1.], [0., 1.]))

    def my_host_fn():
      counter = variable_scope.get_variable(name="host_counter",
                                            initializer=0.0)
      return counter.assign_add(1.0)

    def my_model_fn(features, labels, mode):
      loss = features + labels
      counter = variable_scope.get_variable(name="ipu_counter",
                                            initializer=0.0)
      train_op = counter.assign_add(1.0)
      host_call = (my_host_fn, [])
      return ipu_estimator.IPUEstimatorSpec(mode=mode,
                                            loss=loss,
                                            train_op=train_op,
                                            host_call=host_call)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    estimator.train(input_fn=my_input_fn, steps=2)
    self.assertEqual(2.0, estimator.get_variable_value("host_counter"))
    self.assertEqual(2.0, estimator.get_variable_value("ipu_counter"))

  def testHostCallNotReturningAnything(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensor_slices(([1., 1.], [0., 1.]))

    def my_host_fn():
      pass

    def my_model_fn(features, labels, mode):
      loss = features + labels
      train_op = array_ops.identity(loss)
      host_call = (my_host_fn, [])
      return ipu_estimator.IPUEstimatorSpec(mode=mode,
                                            loss=loss,
                                            train_op=train_op,
                                            host_call=host_call)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    with self.assertRaisesRegexp(
        TypeError, "`host_call` return value must be Operation or Tensor"):
      estimator.train(input_fn=my_input_fn, steps=1)

  def testHostCallMultipleIterationsPerLoopNotAllowed(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensor_slices(([], []))

    def my_host_fn():
      pass

    def my_model_fn(features, labels, mode):
      loss = features + labels
      train_op = array_ops.identity(loss)
      host_call = (my_host_fn, [])
      return ipu_estimator.IPUEstimatorSpec(mode=mode,
                                            loss=loss,
                                            train_op=train_op,
                                            host_call=host_call)

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=2))

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)
    with self.assertRaisesRegexp(
        ValueError, "host_call is not allowed for iterations_per_loop > 1"):
      estimator.train(input_fn=my_input_fn, steps=2)

  def testHostCallTwoArguments(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensor_slices(([1., 1.], [0., 1.]))

    def my_host_fn(features, labels):
      feature_sum = variable_scope.get_variable(name="feature_sum",
                                                initializer=0.0)
      label_sum = variable_scope.get_variable(name="label_sum",
                                              initializer=0.0)
      return control_flow_ops.group(
          [feature_sum.assign_add(features),
           label_sum.assign_add(labels)])

    def my_model_fn(features, labels, mode):
      loss = features + labels
      self.assertEqual("/device:IPU:0", loss.device)
      train_op = array_ops.identity(loss)
      host_call = (my_host_fn, [features, labels])
      return ipu_estimator.IPUEstimatorSpec(mode=mode,
                                            loss=loss,
                                            train_op=train_op,
                                            host_call=host_call)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    estimator.train(input_fn=my_input_fn, steps=2)
    self.assertEqual(2., estimator.get_variable_value("feature_sum"))
    self.assertEqual(1., estimator.get_variable_value("label_sum"))

  def testModelFnDoesNotTakeLabels(self):
    def my_input_fn():
      dataset = tu.create_dual_increasing_dataset(10,
                                                  data_shape=[1],
                                                  label_shape=[1])
      dataset = dataset.batch(batch_size=1, drop_remainder=True)
      return dataset

    def my_model_fn(features, mode):
      loss = math_ops.reduce_sum(features, name="loss")
      train_op = array_ops.identity(loss)
      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    with self.assertRaisesRegexp(ValueError, "model_fn does not take labels"):
      estimator.train(input_fn=my_input_fn, steps=1)

  def testNotPassingDataset(self):
    def my_input_fn():
      return 1, 2

    def my_model_fn(features, labels, mode):
      loss = math_ops.reduce_sum(features + labels, name="loss")
      train_op = array_ops.identity(loss)
      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    with self.assertRaisesRegexp(ValueError, "must return Dataset"):
      estimator.train(input_fn=my_input_fn, steps=1)

  def testVerifyTrainFeeds(self):
    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      loss = math_ops.reduce_sum(features + labels, name="loss")

      train_op = array_ops.identity(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = tu.create_dual_increasing_dataset(10,
                                                  data_shape=[1],
                                                  label_shape=[1])
      dataset = dataset.batch(batch_size=1, drop_remainder=True)
      return dataset

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=1),
        log_step_count_steps=1,
        save_summary_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    session_run_counter = _SessionRunCounter()

    num_steps = 6
    estimator.train(input_fn=my_input_fn,
                    steps=num_steps,
                    hooks=[session_run_counter])

    self.assertEquals(session_run_counter.num_session_runs, num_steps)

    model_dir = estimator.model_dir
    events_file = glob.glob(model_dir + "/*tfevents*")
    assert len(events_file) == 1
    events_file = events_file[0]
    loss_output = list()
    for e in summary_iterator.summary_iterator(events_file):
      for v in e.summary.value:
        if "loss" in v.tag:
          loss_output.append(v.simple_value)

    self.assertEqual(loss_output, list(np.arange(0.0, 12.0, 2.0)))

  def testTrainWithWarmStart(self):
    class IntrospectionHook(session_run_hook.SessionRunHook):
      def __init__(self):
        self._result = {}
        self._trainable_vars = []

      def begin(self):
        self._trainable_vars = ops.get_collection(
            ops.GraphKeys.TRAINABLE_VARIABLES, scope=".*")
        self._result = {v.name[:-2]: None for v in self._trainable_vars}

      def after_create_session(self, session, coord):
        result = session.run(self._trainable_vars)
        for (k, _), r in zip(six.iteritems(self._result), result):
          self._result[k] = r

      @property
      def result(self):
        return self._result

    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      with variable_scope.variable_scope("vs", use_resource=True):
        predictions = layers.Dense(units=1)(features)

      loss = losses.mean_squared_error(labels=labels, predictions=predictions)
      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train_op = optimizer.minimize(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = dataset_ops.Dataset.from_tensor_slices(
          _create_regression_dataset(num_samples=1000, num_features=5))
      dataset = dataset.batch(batch_size=2, drop_remainder=True).repeat()
      return dataset

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=2),
        log_step_count_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    model_dir = estimator.model_dir

    session_run_counter = _SessionRunCounter()

    estimator.train(input_fn=my_input_fn, steps=4, hooks=[session_run_counter])

    variable_names = estimator.get_variable_names()
    estimator_variables = {}
    for n in variable_names:
      if "vs" in n:
        estimator_variables[n] = estimator.get_variable_value(n)

    del estimator
    del session_run_counter
    # Create new estimator which warm starts from previous estimator's output
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=config,
                                           warm_start_from=model_dir)
    session_run_counter = _SessionRunCounter()
    introspect_hook = IntrospectionHook()

    hooks = [session_run_counter, introspect_hook]
    estimator.train(input_fn=my_input_fn, steps=4, hooks=hooks)

    warm_started_estimator_variables = introspect_hook.result

    self.assertEquals(six.viewkeys(estimator_variables),
                      six.viewkeys(warm_started_estimator_variables))

    for k, _ in six.iteritems(estimator_variables):
      self.assertEquals(estimator_variables[k][0],
                        warm_started_estimator_variables[k][0])

  def testCompileSummary(self):
    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      with variable_scope.variable_scope("vs", use_resource=True):
        predictions = layers.Dense(units=1)(features)

      loss = losses.mean_squared_error(labels=labels, predictions=predictions)
      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train_op = optimizer.minimize(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = dataset_ops.Dataset.from_tensor_slices(
          _create_regression_dataset(num_samples=1000, num_features=5))
      dataset = dataset.batch(batch_size=2, drop_remainder=True).repeat()
      return dataset

    ipu_options = ipu_utils.create_ipu_config(profiling=True)

    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             ipu_options=ipu_options,
                                             compile_summary=True)

    run_config = ipu_run_config.RunConfig(ipu_run_config=ipu_config,
                                          log_step_count_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=run_config)

    estimator.train(input_fn=my_input_fn, steps=4)

    event_file = glob.glob(estimator.model_dir + "/event*")
    self.assertTrue(len(event_file) == 1)
    compile_for_ipu_count = 0
    for summary in summary_iterator.summary_iterator(event_file[0]):
      for val in summary.summary.value:
        if val.tag == "ipu_trace":
          for evt_str in val.tensor.string_val:
            evt = IpuTraceEvent.FromString(evt_str)

            if (evt.type == IpuTraceEvent.COMPILE_END
                and len(evt.compile_end.compilation_report) > 0):
              compile_for_ipu_count += 1

    self.assertEqual(compile_for_ipu_count, 1)

  def testEventDecode(self):
    class EventTraceHook(session_run_hook.SessionRunHook):
      def __init__(self):
        self._event_op = None
        self._events = None

      def begin(self):
        self._event_op = gen_ipu_ops.ipu_event_trace()

      def after_run(self, run_context, run_values):
        self._events = run_context.session.run(self._event_op)

      @property
      def events(self):
        return self._events

    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      with variable_scope.variable_scope("vs", use_resource=True):
        predictions = layers.Dense(units=1)(features)

      loss = losses.mean_squared_error(labels=labels, predictions=predictions)
      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train_op = optimizer.minimize(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = dataset_ops.Dataset.from_tensor_slices(
          _create_regression_dataset(num_samples=1000, num_features=5))
      dataset = dataset.batch(batch_size=2, drop_remainder=True).repeat()
      return dataset

    ipu_options = ipu_utils.create_ipu_config(profiling=True)

    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             ipu_options=ipu_options,
                                             compile_summary=True)

    run_config = ipu_run_config.RunConfig(ipu_run_config=ipu_config,
                                          log_step_count_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=run_config)

    event_trace_hook = EventTraceHook()
    hooks = [event_trace_hook]
    estimator.train(input_fn=my_input_fn, steps=4, hooks=hooks)

    events = event_trace_hook.events
    self.assertEqual(len(events), 3)

  def testLossAveraging(self):
    def my_model_fn(features, labels, mode):
      self.assertEquals(model_fn_lib.ModeKeys.TRAIN, mode)

      loss = math_ops.reduce_sum(features + labels, name="loss")

      train_op = array_ops.identity(loss)

      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op)

    def my_input_fn():
      dataset = tu.create_dual_increasing_dataset(10,
                                                  data_shape=[1],
                                                  label_shape=[1])
      dataset = dataset.batch(batch_size=2, drop_remainder=True)
      return dataset

    iterations_per_loop = 2
    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(
            iterations_per_loop=iterations_per_loop),
        log_step_count_steps=1,
        save_summary_steps=1)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    session_run_counter = _SessionRunCounter()

    num_steps = 6
    num_session_runs = num_steps // iterations_per_loop
    estimator.train(input_fn=my_input_fn,
                    steps=num_steps,
                    hooks=[session_run_counter])

    self.assertEquals(session_run_counter.num_session_runs, num_session_runs)

    model_dir = estimator.model_dir
    events_file = glob.glob(model_dir + "/*tfevents*")
    assert len(events_file) == 1
    events_file = events_file[0]
    loss_output = list()
    for e in summary_iterator.summary_iterator(events_file):
      for v in e.summary.value:
        if "loss" in v.tag:
          loss_output.append(v.simple_value)

    self.assertEqual(loss_output, [6.0, 22.0, 18.0])

  def testEvaluate(self):
    def my_input_fn():
      features = [0., 2.]  # mean: 1
      labels = [1., 3.]  # mean: 2
      return dataset_ops.Dataset.from_tensors((features, labels))

    def my_model_fn(features, labels, mode):
      loss = math_ops.reduce_mean(features + labels)
      eval_metric_ops = {
          "feature_mean": metrics_impl.mean(features),
          "label_mean": metrics_impl.mean(labels),
      }
      return model_fn_lib.EstimatorSpec(mode,
                                        loss=loss,
                                        eval_metric_ops=eval_metric_ops)

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=1))
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)
    scores = estimator.evaluate(my_input_fn, steps=1)
    self.assertEqual(1., scores["feature_mean"])
    self.assertEqual(2., scores["label_mean"])
    self.assertEqual(3., scores[model_fn_lib.LOSS_METRIC_KEY])

  def testEvaluateMissingEvalMetrics(self):
    def my_input_fn():
      return dataset_ops.Dataset.from_tensors(([], []))

    def my_model_fn(features, labels, mode):
      loss = math_ops.reduce_mean(features + labels)
      return model_fn_lib.EstimatorSpec(mode, loss=loss)

    config = ipu_run_config.RunConfig()
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)
    with self.assertRaisesRegexp(ValueError, "must contain eval_metric_ops"):
      estimator.evaluate(my_input_fn, steps=1)

  def testPredictTensorTwoPerIteration(self):
    def my_input_fn():
      features = [[2.0], [3.0]]
      return dataset_ops.Dataset.from_tensor_slices(features)

    def my_model_fn(features, labels, mode):
      self.assertIsNone(labels)
      return model_fn_lib.EstimatorSpec(
          mode,
          predictions=features,
      )

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=2))
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=True)
    self.assertAllEqual(2.0, next(outputs))
    self.assertAllEqual(3.0, next(outputs))

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=False)
    self.assertAllEqual([2.0], next(outputs))
    self.assertAllEqual([3.0], next(outputs))

  def testPredictDictOnePerIteration(self):
    def my_input_fn():
      feature = [2.0]
      label = [3.0]
      return dataset_ops.Dataset.from_tensors((feature, label))

    def my_model_fn(features, labels, mode):
      return model_fn_lib.EstimatorSpec(
          mode,
          predictions={
              "features": features,
              "labels": labels,
          },
      )

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=1))
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    output = next(estimator.predict(input_fn=my_input_fn))
    self.assertAllEqual(2.0, output["features"])
    self.assertAllEqual(3.0, output["labels"])

    output = next(
        estimator.predict(input_fn=my_input_fn, predict_keys=["features"]))
    self.assertAllEqual(2.0, output["features"])
    self.assertFalse("labels" in output)

  def testPredictTensorBatchShouldGiveFlattenedOutput(self):
    def my_input_fn():
      features = [
          [2.0, 3.0],
          [3.0, 2.0],
          [3.0, 2.0],
          [2.0, 3.0],
          [2.0, 3.0],
          [3.0, 2.0],
      ]
      return dataset_ops.Dataset.from_tensor_slices(features).batch(
          2, drop_remainder=True)

    def my_model_fn(features, mode):
      return model_fn_lib.EstimatorSpec(
          mode,
          predictions=math_ops.argmax(features,
                                      axis=-1,
                                      output_type=dtypes.int32),
      )

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=3))
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=True)
    self.assertAllEqual(1, next(outputs))
    self.assertAllEqual(0, next(outputs))
    self.assertAllEqual(0, next(outputs))
    self.assertAllEqual(1, next(outputs))
    self.assertAllEqual(1, next(outputs))
    self.assertAllEqual(0, next(outputs))

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=False)
    self.assertAllEqual([1, 0], next(outputs))
    self.assertAllEqual([0, 1], next(outputs))
    self.assertAllEqual([1, 0], next(outputs))

  def testPredictDictBatchShouldGiveFlattenedOutput(self):
    def my_input_fn():
      features = [
          [2.0, 3.0],
          [3.0, 2.0],
          [3.0, 2.0],
          [2.0, 3.0],
          [2.0, 3.0],
          [3.0, 2.0],
      ]
      return dataset_ops.Dataset.from_tensor_slices(features).batch(
          2, drop_remainder=True)

    def my_model_fn(features, mode):
      predictions = {
          "predictions":
          math_ops.argmax(features, axis=-1, output_type=dtypes.int32),
      }
      return model_fn_lib.EstimatorSpec(mode, predictions=predictions)

    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=3))
    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn, config=config)

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=True)
    self.assertAllEqual(1, next(outputs)["predictions"])
    self.assertAllEqual(0, next(outputs)["predictions"])
    self.assertAllEqual(0, next(outputs)["predictions"])
    self.assertAllEqual(1, next(outputs)["predictions"])
    self.assertAllEqual(1, next(outputs)["predictions"])
    self.assertAllEqual(0, next(outputs)["predictions"])

    outputs = estimator.predict(input_fn=my_input_fn,
                                yield_single_examples=False)
    self.assertAllEqual([1, 0], next(outputs)["predictions"])
    self.assertAllEqual([0, 1], next(outputs)["predictions"])
    self.assertAllEqual([1, 0], next(outputs)["predictions"])

  def testStepsMustBeMultipleOfIterationsPerLoop(self):
    config = ipu_run_config.RunConfig(
        ipu_run_config=ipu_run_config.IPURunConfig(iterations_per_loop=3))
    estimator = ipu_estimator.IPUEstimator(model_fn=_dummy_model_fn,
                                           config=config)

    with self.assertRaisesRegexp(ValueError,
                                 "must be a multiple of iterations_per_loop"):
      estimator.train(input_fn=lambda: None, steps=1)

    with self.assertRaisesRegexp(ValueError,
                                 "must be a multiple of iterations_per_loop"):
      estimator.evaluate(input_fn=lambda: None, steps=2)

  def testIPURunConfig(self):
    with self.assertRaisesRegexp(
        ValueError, "configuration requires more than one device"):
      ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                  num_replicas=3,
                                  ipu_options=None,
                                  compile_summary=True)

    with self.assertRaisesRegexp(
        ValueError, "configuration requires more than one device"):
      ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                  num_shards=2,
                                  ipu_options=None,
                                  compile_summary=True)

    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_replicas=1,
                                             ipu_options=None,
                                             compile_summary=True)
    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_shards=1,
                                             ipu_options=None,
                                             compile_summary=True)
    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             ipu_options=ipu_options,
                                             compile_summary=True)
    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    with self.assertRaisesRegexp(ValueError, "`IpuOptions` configured with"):
      ipu_options = ipu_utils.create_ipu_config(profiling=True)
      ipu_options = ipu_utils.auto_select_ipus(ipu_options, 3)
      ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                               ipu_options=ipu_options,
                                               compile_summary=True)

    with self.assertRaisesRegexp(ValueError, "`IpuOptions` configured with"):
      ipu_options = ipu_utils.create_ipu_config(profiling=True)
      ipu_options = ipu_utils.auto_select_ipus(ipu_options, 3)
      ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                               num_replicas=4,
                                               ipu_options=ipu_options,
                                               compile_summary=True)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_options = ipu_utils.auto_select_ipus(ipu_options, 4)
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_replicas=4,
                                             ipu_options=ipu_options,
                                             compile_summary=True)
    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_options = ipu_utils.auto_select_ipus(ipu_options, 4)
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_replicas=2,
                                             num_shards=2,
                                             ipu_options=ipu_options,
                                             compile_summary=True)
    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    with self.assertRaisesRegexp(ValueError, "`IpuOptions` configured with"):
      ipu_options = ipu_utils.create_ipu_config(profiling=True)
      ipu_options = ipu_utils.select_ipus(ipu_options, [0, 1, 2])
      ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                               num_replicas=4,
                                               ipu_options=ipu_options,
                                               compile_summary=True)

    with self.assertRaisesRegexp(ValueError, "`IpuOptions` configured with"):
      ipu_options = ipu_utils.create_ipu_config(profiling=True)
      ipu_options = ipu_utils.select_ipus(ipu_options, [0, 1, 2])
      ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                               num_shards=4,
                                               ipu_options=ipu_options,
                                               compile_summary=True)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_options = ipu_utils.select_ipus(ipu_options, [0, 1, 2, 3])
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_shards=4,
                                             ipu_options=ipu_options,
                                             compile_summary=True)

    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_options = ipu_utils.select_ipus(ipu_options, [0, 1, 2, 3])
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_replicas=2,
                                             num_shards=2,
                                             ipu_options=ipu_options,
                                             compile_summary=True)

    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

    ipu_options = ipu_utils.create_ipu_config(profiling=True)
    ipu_options = ipu_utils.select_ipus(ipu_options, [0])
    ipu_config = ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                             num_replicas=1,
                                             num_shards=1,
                                             ipu_options=ipu_options,
                                             compile_summary=True)

    self.assertIsInstance(ipu_config, ipu_run_config.IPURunConfig)

  def testDatasetWithDicts(self):
    def my_input_fn():
      features = {
          "x0": np.array([[1.0]], dtype=np.float32),
          "x1": np.array([[2.0]], dtype=np.float32)
      }
      labels = {
          "y0": np.array([[3.0]], dtype=np.float32),
          "y1": np.array([[4.0]], dtype=np.float32)
      }
      dataset = dataset_ops.Dataset.from_tensor_slices((features, labels))
      return dataset.batch(1, drop_remainder=True)

    def my_model_fn(features, labels, mode):
      loss = math_ops.reduce_sum(features["x0"] + features["x1"] +
                                 labels["y0"] + labels["y1"],
                                 axis=-1)
      train_op = array_ops.identity(loss)
      predictions = loss
      eval_metric_ops = {"mean_loss": metrics_impl.mean(loss)}
      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op,
                                        predictions=predictions,
                                        eval_metric_ops=eval_metric_ops)

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    # train
    estimator.train(input_fn=my_input_fn, steps=1)

    # predict
    predictions = estimator.predict(input_fn=my_input_fn)
    self.assertEquals(10.0, next(predictions))

    # evaluate
    scores = estimator.evaluate(my_input_fn, steps=1)
    self.assertAllClose(10.0, scores["loss"])

  def testPassingHooksFromModelFunction(self):
    def my_input_fn():
      features = np.array([[1.0]], dtype=np.float32)
      labels = np.array([[2.0]], dtype=np.float32)
      dataset = dataset_ops.Dataset.from_tensor_slices((features, labels))
      return dataset.batch(1, drop_remainder=True)

    training_hook = _SessionRunCounter()
    evaluation_hook = _SessionRunCounter()
    prediction_hook = _SessionRunCounter()

    def my_model_fn(features, labels, mode):
      loss = features + labels
      train_op = array_ops.identity(loss)
      predictions = loss
      eval_metric_ops = {"mean_loss": metrics_impl.mean(loss)}
      return model_fn_lib.EstimatorSpec(mode=mode,
                                        loss=loss,
                                        train_op=train_op,
                                        predictions=predictions,
                                        eval_metric_ops=eval_metric_ops,
                                        training_hooks=[training_hook],
                                        evaluation_hooks=[evaluation_hook],
                                        prediction_hooks=[prediction_hook])

    estimator = ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                           config=ipu_run_config.RunConfig())

    # train
    self.assertEquals(0, training_hook.num_session_runs)
    estimator.train(input_fn=my_input_fn, steps=1)
    self.assertEquals(1, training_hook.num_session_runs)

    # predict: not evaluated before generator is consumed
    self.assertEquals(0, prediction_hook.num_session_runs)
    predictions = estimator.predict(input_fn=my_input_fn)
    self.assertEquals(0, prediction_hook.num_session_runs)
    next(predictions)
    self.assertEquals(1, prediction_hook.num_session_runs)

    # evaluate
    self.assertEquals(0, evaluation_hook.num_session_runs)
    estimator.evaluate(my_input_fn, steps=1)
    self.assertEquals(1, evaluation_hook.num_session_runs)


if __name__ == "__main__":
  googletest.main()
