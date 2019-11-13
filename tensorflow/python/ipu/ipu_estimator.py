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
# ===================================================================
"""
IPUEstimator
~~~~~~~~~~~~
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import six

from tensorflow.compiler.plugin.poplar.driver.config_pb2 import IpuOptions
from tensorflow.compiler.plugin.poplar.ops import gen_sendrecv_ops
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.distribute import distribution_strategy_context
from tensorflow.python.estimator import estimator as estimator_lib
from tensorflow.python.estimator import model_fn as model_fn_lib
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ipu import autoshard
from tensorflow.python.ipu import ipu_compiler
from tensorflow.python.ipu import ipu_infeed_queue
from tensorflow.python.ipu import ipu_outfeed_queue
from tensorflow.python.ipu import ipu_run_config
from tensorflow.python.ipu import loops
from tensorflow.python.ipu import ops as ipu_ops
from tensorflow.python.ipu import utils as ipu_utils
from tensorflow.python.ipu.ipu_multi_worker_strategy import IPUMultiWorkerStrategy
from tensorflow.python.ipu.ipu_outfeed_queue import IPUOutfeedMode
from tensorflow.python.ipu.scopes import ipu_scope
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.training import session_run_hook
from tensorflow.python.training import training_util
from tensorflow.python.util import function_utils

_INITIAL_LOSS = 0.0
_INPUT_FN_KEY = "input_fn"
_CROSS_REPLICA_SUM_OP = "IpuCrossReplicaSum"
_HOST_DEVICE = "/device:CPU:0"
_IPU_DEVICE = "/device:IPU:0"

# Keys that cannot be used in the `params` dictionary passed to the
# IPUEstimator
_RESERVED_PARAMS_KEYS = [_INPUT_FN_KEY]


def _next_feed_id():
  result = "feed" + str(_next_feed_id.feed_count)
  _next_feed_id.feed_count += 1
  return result


_next_feed_id.feed_count = 0


class IPUEstimatorSpec(
    collections.namedtuple('IPUEstimatorSpec', [
        'mode', 'predictions', 'loss', 'train_op', 'eval_metrics', 'host_call',
        'training_hooks', 'evaluation_hooks', 'prediction_hooks'
    ])):
  """Ops and objects returned from a `model_fn` and passed to `IPUEstimator`."""
  def __new__(cls,
              mode,
              predictions=None,
              loss=None,
              train_op=None,
              eval_metrics=None,
              host_call=None,
              training_hooks=None,
              evaluation_hooks=None,
              prediction_hooks=None):
    train_op = model_fn_lib._validate_estimator_spec_train_op(train_op, mode)
    loss = model_fn_lib._validate_estimator_spec_loss(loss, mode)
    predictions = model_fn_lib._validate_estimator_spec_predictions(
        predictions, mode)
    training_hooks = model_fn_lib._validate_estimator_spec_hooks(
        training_hooks)
    evaluation_hooks = model_fn_lib._validate_estimator_spec_hooks(
        evaluation_hooks)
    prediction_hooks = model_fn_lib._validate_estimator_spec_hooks(
        prediction_hooks)

    if host_call is not None:
      if not isinstance(host_call, tuple):
        raise ValueError("`host_call` must ba a tuple")
      if len(host_call) != 2:
        raise ValueError("`host_call` must have two elements")
      if not isinstance(host_call[1], list):
        raise ValueError("second element in `host_call` must be a list")

    return super(IPUEstimatorSpec,
                 cls).__new__(cls,
                              mode=mode,
                              predictions=predictions,
                              loss=loss,
                              train_op=train_op,
                              eval_metrics=eval_metrics,
                              host_call=host_call,
                              training_hooks=training_hooks,
                              evaluation_hooks=evaluation_hooks,
                              prediction_hooks=prediction_hooks)


class _IPUConfigureIPUSystemHook(session_run_hook.SessionRunHook):
  def __init__(self, config, host_device=_HOST_DEVICE):
    if not isinstance(config, IpuOptions):
      raise Exception("`config` must be an IpuOptions instance")
    self._config = config
    self._host_device = host_device

  def begin(self):
    ipu_utils.configure_ipu_system(self._config, self._host_device)


class _IPUInfeedInitializerSessionHook(session_run_hook.SessionRunHook):
  def __init__(self, infeed):
    self._infeed = infeed

  def after_create_session(self, session, coord):
    session.run(self._infeed.initializer)


class _IPUGlobalStepCounterAndStopHook(session_run_hook.SessionRunHook):
  def __init__(self, iterations_per_loop, num_steps, final_step):
    if num_steps is None and final_step is None:
      raise ValueError("One of `num_steps` or `final_step` must be specified.")
    if num_steps is not None and final_step is not None:
      raise ValueError(
          "Only one of `num_steps` or `final_step` can be specified.")

    self._iterations_per_loop = iterations_per_loop
    self._num_steps = num_steps
    self._final_step = final_step

  def after_create_session(self, session, coord):
    global_step = session.run(self._global_step_tensor)
    if self._final_step is None:
      self._final_step = global_step + self._num_steps

  def begin(self):
    self._global_step_tensor = training_util.get_global_step()
    with ops.device(_HOST_DEVICE):
      self._increment_op = self._global_step_tensor.assign_add(
          self._iterations_per_loop)

  def after_run(self, run_context, run_values):
    global_step = run_context.session.run(self._increment_op)
    if global_step >= self._final_step:
      run_context.request_stop()


def _call_input_fn(input_fn, mode, params, config, input_context):
  input_fn_args = function_utils.fn_args(input_fn)
  kwargs = {}
  if "mode" in input_fn_args:
    kwargs["mode"] = mode
  if "params" in input_fn_args:
    kwargs["params"] = params
  if "config" in input_fn_args:
    kwargs["config"] = config
  if input_context and "input_context" in input_fn_args:
    kwargs["input_context"] = input_context
  return input_fn(**kwargs)


def _validate_replicated_training_graph():
  operations = ops.get_default_graph().get_operations()
  if not any(o.type == _CROSS_REPLICA_SUM_OP for o in operations):
    raise ValueError(
        ("This is not a valid replicated training graph because no {} " +
         "operations were found. Did you remember to use the " +
         "`tensorflow.python.ipu.ipu_optimizer.CrossReplicaOptimizer`?"
         ).format(_CROSS_REPLICA_SUM_OP))


def _add_send_to_host_ops(tensors):
  """Returns attributes for matching recv ops"""
  recv_ops_attrs = []

  for tensor in tensors:
    model_fn_lib._check_is_tensor_or_operation(  # pylint: disable=protected-access
        tensor, "`host_call` argument")

    attrs = dict(tensor_name=tensor.name,
                 send_device=_IPU_DEVICE,
                 send_device_incarnation=0,
                 recv_device=_HOST_DEVICE)

    gen_sendrecv_ops.ipu_send_to_host(tensor, **attrs)

    # The recv op has an additional type argument.
    attrs["T"] = tensor.dtype
    recv_ops_attrs.append(attrs)

  return recv_ops_attrs


def _add_recv_at_host_ops(recv_ops_attrs):
  tensors = []
  for attrs in recv_ops_attrs:
    tensors.append(gen_sendrecv_ops.ipu_recv_at_host(**attrs))
  return tensors


def _unpack_features_and_labels(args, kwargs):
  if args and kwargs:
    raise ValueError("Invalid dataset with both tuple and keywords")
  if not args and not kwargs:
    raise ValueError("Invalid dataset with neither tuple nor keywords")

  if args:
    if len(args) == 1:
      features = args[0]
      labels = None
    elif len(args) == 2:
      features, labels = args
    else:
      raise ValueError(
          "Invalid dataset tuple, expected 1 or 2 elements, got {}".format(
              len(args)))
  else:
    features = kwargs
    labels = None

  return features, labels


class _ModelFnWrapper(object):
  def __init__(self, model_fn, config, params, infeed_queue):
    self._model_fn = model_fn
    self._config = config
    self._params = params
    self._infeed_queue = infeed_queue
    self._iterations_per_loop = config.ipu_run_config.iterations_per_loop
    self._replication_factor = config.ipu_run_config.num_replicas
    self._num_shards = config.ipu_run_config.num_shards
    self._autosharding = config.ipu_run_config.autosharding
    self._captured_hooks = []
    self._captured_host_call_fn = None
    self._captured_host_call_args = None

  def _loop_replica_mean(self, loop_sum):
    if self._replication_factor == 1:
      return loop_sum / self._iterations_per_loop

    loop_replica_sum = ipu_ops.cross_replica_ops.cross_replica_sum(loop_sum)
    return loop_replica_sum / (self._iterations_per_loop *
                               self._replication_factor)

  def _capture_hooks(self, hooks):
    if hooks:
      assert not self._captured_hooks, "Can only capture hooks once"
      self._captured_hooks = hooks

  @property
  def captured_hooks(self):
    return self._captured_hooks

  def _capture_host_call(self, host_call):
    if host_call:
      assert self._captured_host_call_fn is None, "Can only capture host_call once"
      self._captured_host_call_fn, tensors = host_call
      self._captured_host_call_args = _add_send_to_host_ops(tensors)

  @property
  def captured_host_call_fn(self):
    return self._captured_host_call_fn

  @property
  def captured_host_call_args(self):
    return _add_recv_at_host_ops(self._captured_host_call_args)

  def create_training_loop(self):
    def training_step(total_loss, *args, **kwargs):
      features, labels = _unpack_features_and_labels(args, kwargs)
      estimator_spec = self._call_model_fn(features, labels,
                                           model_fn_lib.ModeKeys.TRAIN)

      loss = estimator_spec.loss
      if loss is None:
        raise ValueError("EstimatorSpec must contain loss when training")

      train_op = estimator_spec.train_op
      if train_op is None:
        raise ValueError("EstimatorSpec must contain train_op when training")

      self._capture_hooks(estimator_spec.training_hooks)

      if isinstance(estimator_spec, IPUEstimatorSpec):
        self._capture_host_call(estimator_spec.host_call)

      if self._autosharding:
        autoshard.automatic_sharding(self._num_shards, features, loss)

      # training_step will be run by xla.compile(). xla.compile() only supports
      # tensor output while train_op can be either an operation or a tensor.
      # Even though xla.compile() automatically adds operation-typed train_op as
      # control dependency of other tensor outputs, it doesn"t do so for
      # tensor-typed train_op. Thus, we need to set it explicitly here.
      with ops.control_dependencies([train_op]):
        total_loss += math_ops.cast(loss, dtypes.float32)

      if self._replication_factor > 1:
        _validate_replicated_training_graph()

      return total_loss

    def training_loop():
      if self._iterations_per_loop == 1:
        # Simplify the graph by avoiding the loop.
        inputs = self._infeed_queue._dequeue()  # pylint: disable=protected-access
        args, kwargs = loops._body_arguments(inputs)  # pylint: disable=protected-access
        total_loss = training_step(_INITIAL_LOSS, *args, **kwargs)
        return total_loss

      total_loss = loops.repeat(self._iterations_per_loop,
                                training_step,
                                inputs=[_INITIAL_LOSS],
                                infeed_queue=self._infeed_queue)

      if self._captured_host_call_fn is not None:
        # TODO(hakons): Could use outfeed queue to implement this.
        raise ValueError(
            "host_call is not allowed for iterations_per_loop > 1")

      return self._loop_replica_mean(total_loss)

    return training_loop

  def create_evaluation_loop(self, outfeed_queue):
    def evaluation_step(total_loss, *args, **kwargs):
      features, labels = _unpack_features_and_labels(args, kwargs)
      estimator_spec = self._call_model_fn(features, labels,
                                           model_fn_lib.ModeKeys.EVAL)

      loss = estimator_spec.loss
      if loss is None:
        raise ValueError("EstimatorSpec must contain loss when evaluating")

      eval_metric_ops = estimator_spec.eval_metric_ops
      if not eval_metric_ops:
        raise ValueError(
            "EstimatorSpec must contain eval_metric_ops when evaluating")

      self._capture_hooks(estimator_spec.evaluation_hooks)

      update_op, value_ops = estimator_lib._extract_metric_update_ops(  # pylint: disable=protected-access
          eval_metric_ops)

      if self._autosharding:
        autoshard.automatic_sharding(self._num_shards, features, loss)

      with ops.control_dependencies([update_op, loss]):
        total_loss += math_ops.cast(loss, dtypes.float32)
        outfeed = outfeed_queue.enqueue(value_ops)
        return total_loss, outfeed

    def evaluation_loop():
      total_loss = loops.repeat(self._iterations_per_loop,
                                evaluation_step,
                                inputs=[_INITIAL_LOSS],
                                infeed_queue=self._infeed_queue)
      return self._loop_replica_mean(total_loss)

    return evaluation_loop

  def create_prediction_loop(self, outfeed_queue):
    def prediction_step(*args, **kwargs):
      features, _ = _unpack_features_and_labels(args, kwargs)
      labels = None  # Do not provide labels for prediction
      estimator_spec = self._call_model_fn(features, labels,
                                           model_fn_lib.ModeKeys.PREDICT)

      predictions = estimator_spec.predictions
      if predictions is None:
        raise ValueError(
            "EstimatorSpec must contain predictions when predicting")

      self._capture_hooks(estimator_spec.prediction_hooks)

      outfeed = outfeed_queue.enqueue(predictions)
      return outfeed

    def prediction_loop():
      return loops.repeat(self._iterations_per_loop,
                          prediction_step,
                          infeed_queue=self._infeed_queue)

    return prediction_loop

  def _call_model_fn(self, features, labels, mode):
    model_fn_args = function_utils.fn_args(self._model_fn)
    kwargs = {}
    if "labels" in model_fn_args:
      kwargs["labels"] = labels
    else:
      if labels is not None:
        raise ValueError(
            "model_fn does not take labels, but input_fn returns labels.")
    if "mode" in model_fn_args:
      kwargs["mode"] = mode
    if "params" in model_fn_args:
      kwargs["params"] = self._params
    if "config" in model_fn_args:
      kwargs["config"] = self._config

    estimator_spec = self._model_fn(features=features, **kwargs)

    valid_classes = (IPUEstimatorSpec, model_fn_lib.EstimatorSpec)
    if not isinstance(estimator_spec, valid_classes):
      raise ValueError("`model_fn` must return {}".format(" or ".join(
          [cls.__name__ for cls in valid_classes])))

    return estimator_spec


def _call_host_fn(host_call_fn, host_call_args):
  assert host_call_fn is not None
  assert host_call_args is not None

  with ops.device(_HOST_DEVICE):
    ret = host_call_fn(*host_call_args)

  model_fn_lib._check_is_tensor_or_operation(  # pylint: disable=protected-access
      ret, "`host_call` return value")

  return ret


def _get_input_context():
  strategy = distribution_strategy_context.get_strategy()
  if isinstance(strategy, IPUMultiWorkerStrategy):
    return strategy.extended._make_input_context()  # pylint: disable=protected-access
  return None


def _augment_model_fn(model_fn):
  """Returns a new model_fn, which wraps the IPU support."""
  def _model_fn(features, labels, mode, config, params):
    del features, labels  # We call the input_fn directly from here instead
    input_fn = params[_INPUT_FN_KEY]
    input_context = _get_input_context()
    dataset = _call_input_fn(input_fn, mode, params, config, input_context)

    if not isinstance(dataset, dataset_ops.Dataset):
      raise ValueError("input_fn must return Dataset")

    replication_factor = config.ipu_run_config.num_replicas
    infeed_queue = ipu_infeed_queue.IPUInfeedQueue(
        dataset, _next_feed_id(), replication_factor=replication_factor)

    hooks = [
        _IPUInfeedInitializerSessionHook(infeed_queue),
    ]

    if config.ipu_run_config.ipu_options is None:
      if config.ipu_run_config.compile_summary:
        logging.warning(
            "Compile summary enabled but IpuOptions is None. No profile will be generated"
        )

    if config.ipu_run_config.ipu_options is not None:
      ipu_options = config.ipu_run_config.ipu_options
      hooks.append(
          _IPUConfigureIPUSystemHook(ipu_options, host_device=_HOST_DEVICE))

    model_fn_wrapper = _ModelFnWrapper(model_fn, config, params, infeed_queue)

    if mode == model_fn_lib.ModeKeys.TRAIN:
      loop = model_fn_wrapper.create_training_loop()
    elif mode == model_fn_lib.ModeKeys.EVAL:
      outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue(
          "eval_" + _next_feed_id(),
          outfeed_mode=IPUOutfeedMode.LAST,
          replication_factor=replication_factor)
      loop = model_fn_wrapper.create_evaluation_loop(outfeed_queue)
    elif mode == model_fn_lib.ModeKeys.PREDICT:
      outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue(
          "predict_" + _next_feed_id(),
          outfeed_mode=IPUOutfeedMode.ALL,
          replication_factor=replication_factor)
      loop = model_fn_wrapper.create_prediction_loop(outfeed_queue)
    else:
      raise ValueError("Unknown mode: {}".format(mode))

    with ipu_scope(_IPU_DEVICE):
      compiled_loop = ipu_compiler.compile(loop)

    if config.ipu_run_config.compile_summary:
      ipu_ops.summary_ops.ipu_compile_summary("compile_summary", compiled_loop)

    ipu_utils.move_variable_initialization_to_cpu()

    hooks.extend(model_fn_wrapper.captured_hooks)

    loss = None
    train_op = None
    predictions = None
    eval_metric_ops = {}

    if mode == model_fn_lib.ModeKeys.TRAIN:
      loss = compiled_loop[0]
      if model_fn_wrapper.captured_host_call_fn is None:
        train_op = loss
      else:
        # The base class will run both `train_op` and `loss`.
        # Let `train_op` be the return value from the host call.
        # If there is a dependency on the `loss` calculated on
        # the IPU, they will be sequenced. Otherwise they might
        # run in parallel on the IPU and CPU.
        train_op = _call_host_fn(model_fn_wrapper.captured_host_call_fn,
                                 model_fn_wrapper.captured_host_call_args)
    elif mode == model_fn_lib.ModeKeys.PREDICT:
      with ops.control_dependencies([compiled_loop]):
        predictions = outfeed_queue.dequeue()
    elif mode == model_fn_lib.ModeKeys.EVAL:
      loss = compiled_loop[0]
      dequeue_op = outfeed_queue.dequeue()
      for metric_name, metric_tensor in six.iteritems(dequeue_op):
        # TODO(hakons): mean is not always correct, e.g. for root_mean_squared_error
        cross_replica_metric = math_ops.reduce_mean(metric_tensor)
        # No op as update-op since updating is done inside the loop
        eval_metric_ops[metric_name] = (cross_replica_metric,
                                        control_flow_ops.no_op())

    return model_fn_lib.EstimatorSpec(mode=mode,
                                      loss=loss,
                                      train_op=train_op,
                                      training_hooks=hooks,
                                      evaluation_hooks=hooks,
                                      prediction_hooks=hooks,
                                      eval_metric_ops=eval_metric_ops,
                                      predictions=predictions)

  return _model_fn


class IPUEstimator(estimator_lib.Estimator):
  """Estimator with IPU support.

  IPUEstimator handles many of the details of running on IPUs, such as
  placement of operations and tensors, graph compilation and usage of
  data feeds. It also provides a simple way to use multiple IPUs in the
  form of either data parallelism or model parallelism.

  For efficiency, it supports compiling a graph that contains multiple
  iterations of the training/prediction/evaluation loop, which will be
  fully executed on the IPU before yielding back to the TensorFlow
  Python runtime on the CPU.

  See https://tensorflow.org/guide/estimators for general information
  about estimators.

  Args:
    model_fn: The model function. Refer to
      https://www.tensorflow.org/guide/custom_estimators#write_a_model_function
      for details on how to write this function.
    model_dir: Directory to save model parameters, graph and etc. This can
      also be used to load checkpoints from the directory into an estimator to
      continue training a previously saved model. If `PathLike` object, the
      path will be resolved. If `None`, the model_dir in `config` will be used
      if set. If both are set, they must be same. If both are `None`, a
      temporary directory will be used.
    config: `tf.ipu.ipu_run_config.RunConfig` configuration object.
    params: `dict` of hyper parameters that will be passed into `model_fn`.
            Keys are names of parameters, values are basic python types.
    warm_start_from: Optional string filepath to a checkpoint or SavedModel to
                     warm-start from, or a `tf.estimator.WarmStartSettings`
                     object to fully configure warm-starting.  If the string
                     filepath is provided instead of a
                     `tf.estimator.WarmStartSettings`, then all variables are
                     warm-started, and it is assumed that vocabularies
                     and `tf.Tensor` names are unchanged.
  """
  def __init__(self,
               model_fn,
               model_dir=None,
               config=None,
               params=None,
               warm_start_from=None):
    # Base Estimator does not allow for overriding publice APIs as of June 2019
    estimator_lib.Estimator._assert_members_are_not_overridden = lambda _: None

    if config is None or not isinstance(config, ipu_run_config.RunConfig):
      raise ValueError(
          "`config` must be provided with type `ipu_run_config.RunConfig`")

    # Verifies the model_fn signature according to Estimator framework.
    estimator_lib._verify_model_fn_args(model_fn, params)  # pylint: disable=protected-access

    if params is not None and not isinstance(params, dict):
      raise ValueError('`params` is expected to be of type `dict`')
    if params is not None and any(k in params for k in _RESERVED_PARAMS_KEYS):
      raise ValueError('{} are reserved keys but existed in params {}.'.format(
          _RESERVED_PARAMS_KEYS, params))

    model_function = _augment_model_fn(model_fn)

    super(IPUEstimator, self).__init__(model_fn=model_function,
                                       model_dir=model_dir,
                                       config=config,
                                       params=params,
                                       warm_start_from=warm_start_from)

  def train(self,
            input_fn,
            hooks=None,
            steps=None,
            max_steps=None,
            saving_listeners=None):
    """Trains a model given training data `input_fn`.

    Args:
      input_fn: A function that provides input data for training as minibatches.
        The function should return a `tf.data.Dataset` object. The outputs of
        the `Dataset` object must be a tuple `(features, labels)` where

          * `features` is a `tf.Tensor` or a dictionary of string feature name to `Tensor`
          * `labels` is a `Tensor` or a dictionary of string label name to `Tensor`

        Both `features` and `labels` are consumed by `model_fn`.
      hooks: List of `tf.train.SessionRunHook` subclass instances. Used for
        callbacks inside the training loop.
      steps: Number of steps for which to train the model. `steps` works
        incrementally. If you call two times `train(steps=10)` then training
        occurs in total 20 steps. If you don't want to have incremental behavior
        please set `max_steps` instead. If set, `max_steps` must be `None`.
      max_steps: Number of total steps for which to train model. If set,
        `steps` must be `None`. Two calls to `train(steps=100)` means 200
        training iterations. On the other hand, two calls to `train(max_steps=100)`
        means that the second call will not do any iteration since first call did all
        100 steps.
      saving_listeners: list of `CheckpointSaverListener` objects. Used for
        callbacks that run immediately before or after checkpoint savings.

    Returns:
      `self`, for chaining.
    """
    self._validate_steps(steps)
    self._params[_INPUT_FN_KEY] = input_fn
    return super(IPUEstimator, self).train(input_fn=input_fn,
                                           hooks=hooks,
                                           steps=steps,
                                           max_steps=max_steps,
                                           saving_listeners=saving_listeners)

  def _convert_train_steps_to_hooks(self, steps, max_steps):
    return [
        _IPUGlobalStepCounterAndStopHook(
            self._config.ipu_run_config.iterations_per_loop, steps, max_steps)
    ]

  def _convert_eval_steps_to_hooks(self, steps):
    return self._convert_train_steps_to_hooks(steps, max_steps=None)

  def _validate_steps(self, steps):
    iterations_per_loop = self.config.ipu_run_config.iterations_per_loop
    if steps is not None and steps % iterations_per_loop != 0:
      raise ValueError(
          "steps ({}) must be a multiple of iterations_per_loop ({})".format(
              steps, iterations_per_loop))

  def evaluate(self,
               input_fn,
               steps=None,
               hooks=None,
               checkpoint_path=None,
               name=None):
    """Evaluates the model given evaluation data `input_fn`.

    Args:
      input_fn: A function that constructs the input data for evaluation.
        The function should return a `tf.data.Dataset` object. The outputs of
        the `Dataset` object must be a tuple `(features, labels)` where

          * `features` is a `tf.Tensor` or a dictionary of string feature name to `Tensor`
          * `labels` is a `Tensor` or a dictionary of string label name to `Tensor`

        Both `features` and `labels` are consumed by `model_fn`.
      steps: Number of steps for which to evaluate model.
      hooks: List of `tf.train.SessionRunHook` subclass instances. Used for
        callbacks inside the evaluation call.
      checkpoint_path: Path of a specific checkpoint to evaluate. If `None`, the
        latest checkpoint in `model_dir` is used.  If there are no checkpoints
        in `model_dir`, evaluation is run with newly initialized `Variables`
        instead of ones restored from checkpoint.
      name: Name of the evaluation if user needs to run multiple evaluations on
        different data sets, such as on training data vs test data. Metrics for
        different evaluations are saved in separate folders, and appear
        separately in tensorboard.

    Returns:
      A dict containing the evaluation metrics specified in `model_fn` keyed by
      name, as well as an entry `global_step` which contains the value of the
      global step for which this evaluation was performed.
    """

    self._validate_steps(steps)
    self._params[_INPUT_FN_KEY] = input_fn
    return super(IPUEstimator, self).evaluate(input_fn=input_fn,
                                              hooks=hooks,
                                              steps=steps,
                                              checkpoint_path=checkpoint_path,
                                              name=name)

  def predict(self,
              input_fn,
              predict_keys=None,
              hooks=None,
              checkpoint_path=None,
              yield_single_examples=True):
    """Yields predictions for given features.

    Note: The returned generator will block forever if you try to consume
    more elements than what is generated, instead of raising the regular
    `StopIteration` exception. This is caused by the current behaviour
    when requesting to run a loop on the IPU for more iterations than there
    are elements remaining in the dataset. So you cannot simply drain it by
    using :code:`list(predictions)`, you have to consume the expected number of
    elements, e.g. using :code:`[next(predictions) for _ in range(num_examples)]`.

    Args:
      input_fn: A function that constructs the features. The function should
        return a `tf.data.Dataset` object. The outputs of the `Dataset` object
        should be one of the following:

          * features: A `Tensor` or a dictionary of string feature name to
            `Tensor`. features are consumed by `model_fn`.
          * A tuple, in which case the first item is extracted as features.

      predict_keys: list of `str`, name of the keys to predict. It is used if
        the `tf.estimator.EstimatorSpec.predictions` is a `dict`. If
        `predict_keys` is used then rest of the predictions will be filtered
        from the dictionary. If `None`, returns all.
      hooks: List of `tf.train.SessionRunHook` subclass instances. Used for
        callbacks inside the prediction call.
      checkpoint_path: Path of a specific checkpoint to predict. If `None`, the
        latest checkpoint in `model_dir` is used.  If there are no checkpoints
        in `model_dir`, prediction is run with newly initialized `Variables`
        instead of ones restored from checkpoint.
      yield_single_examples: If `False`, yields the whole batch as returned by
        the `model_fn` instead of decomposing the batch into individual
        elements. This is useful if `model_fn` returns some tensors whose first
        dimension is not equal to the batch size.

    Yields:
      Evaluated values of `predictions` tensors.
    """

    self._params[_INPUT_FN_KEY] = input_fn
    predictions = super(IPUEstimator, self).predict(
        input_fn=input_fn,
        predict_keys=predict_keys,
        hooks=hooks,
        checkpoint_path=checkpoint_path,
        yield_single_examples=yield_single_examples)

    # If yield_single_examples == True, the base class has
    # already flattened the outermost iterations_per_loop
    # dimension, but we also want to flatten the batch dimension.
    # If however yield_single_examples == False, we need to
    # flatten the iterations_per_loop dimension ourselves.
    # So in both cases we need to flatten the output here.
    for nested_predictions in predictions:
      if isinstance(nested_predictions, dict):
        for i in range(self._extract_batch_length(nested_predictions)):
          yield {
              key: value[i]
              for key, value in six.iteritems(nested_predictions)
          }
      else:
        for prediction in nested_predictions:
          yield prediction
