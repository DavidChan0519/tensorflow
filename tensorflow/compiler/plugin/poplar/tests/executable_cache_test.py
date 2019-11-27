import contextlib
import glob
import multiprocessing
import numpy as np
import os
import tempfile
import tensorflow.compiler.plugin.poplar.tests.test_utils as tu

from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_sendrecv_ops
from tensorflow.compiler.tests import xla_test
from tensorflow.core.framework import summary_pb2
from tensorflow.python import ipu
from tensorflow.python import ops
from tensorflow.python.client import session
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.estimator import model_fn
from tensorflow.python.framework import test_util
from tensorflow.python.ipu.ops.summary_ops import ipu_compile_summary
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import metrics_impl
from tensorflow.python.platform import googletest
from tensorflow.python.platform import test
from tensorflow.python.summary import summary_iterator


@contextlib.contextmanager
def _temporary_executable_cache():
  with tempfile.TemporaryDirectory() as temp_dir:
    # Use a nonexistent subdirectory that must be created
    cache_dir = os.path.join(temp_dir, "cache")
    poplar_flags = "--executable_cache_path={} {}".format(
        cache_dir, os.environ.get("TF_POPLAR_FLAGS", ""))
    with test.mock.patch.dict("os.environ", {"TF_POPLAR_FLAGS": poplar_flags}):
      yield


def _run_in_new_process(fn):
  q = multiprocessing.Queue()
  p = multiprocessing.Process(target=lambda: q.put(fn()))
  p.start()
  p.join()
  return q.get()


def _count_ipu_compilations_in_summary(summary):
  count = 0
  for val in summary.value:
    if val.tag == "ipu_trace":
      for evt_str in val.tensor.string_val:
        evt = IpuTraceEvent.FromString(evt_str)
        if (evt.type == IpuTraceEvent.COMPILE_END
            and evt.compile_end.compilation_report):
          count += 1
  return count


def _count_ipu_compilations_in_dir(model_dir):
  count = 0
  for event_file in glob.glob(os.path.join(model_dir, "event*")):
    for event in summary_iterator.summary_iterator(event_file):
      count += _count_ipu_compilations_in_summary(event.summary)
  return count


def _count_ipu_compilations(summary_string):
  summary = summary_pb2.Summary()
  summary.ParseFromString(summary_string)
  return _count_ipu_compilations_in_summary(summary)


class TestExecutableCache(xla_test.XLATestCase):  # pylint: disable=abstract-method
  @test_util.deprecated_graph_mode_only
  def test_basic_model(self):
    def build_and_run_model():
      def my_net(x):
        return x * x

      v = array_ops.placeholder(dtype=np.float32, shape=())
      with ipu.scopes.ipu_scope("/device:IPU:0"):
        [result] = ipu.ipu_compiler.compile(my_net, inputs=[v])
      compile_report = ipu_compile_summary("compile_report", result)

      tu.configure_ipu_system()
      with session.Session() as sess:
        return sess.run([result, compile_report], feed_dict={v: 2.0})

    with _temporary_executable_cache():
      result0, report0 = _run_in_new_process(build_and_run_model)
      result1, report1 = _run_in_new_process(build_and_run_model)
      self.assertEqual(result0, result1)
      self.assertEqual(1, _count_ipu_compilations(report0))
      self.assertEqual(0, _count_ipu_compilations(report1))

  @test_util.deprecated_graph_mode_only
  def test_model_with_infeed_and_outfeed(self):
    def build_and_run_model():
      dataset = dataset_ops.Dataset.from_tensor_slices(
          np.ones(10, dtype=np.float32))
      infeed_queue = ipu.ipu_infeed_queue.IPUInfeedQueue(dataset, "infeed")
      outfeed_queue = ipu.ipu_outfeed_queue.IPUOutfeedQueue("outfeed")

      def body(v, x):
        v = v + x
        outfed = outfeed_queue.enqueue(v)
        return v, outfed

      def my_net(v):
        return ipu.loops.repeat(10, body, v, infeed_queue)

      v = array_ops.placeholder(np.float32, shape=())
      with ipu.scopes.ipu_scope("/device:IPU:0"):
        [result] = ipu.ipu_compiler.compile(my_net, inputs=[v])
      compile_report = ipu_compile_summary("compile_report", result)
      with ops.control_dependencies([result]):
        dequeued = outfeed_queue.dequeue()

      tu.configure_ipu_system()
      with session.Session() as sess:
        sess.run(infeed_queue.initializer)
        return sess.run([result, dequeued, compile_report], {v: 0.0})

    with _temporary_executable_cache():
      result0, dequeued0, report0 = _run_in_new_process(build_and_run_model)
      result1, dequeued1, report1 = _run_in_new_process(build_and_run_model)
      self.assertAllEqual(dequeued0, dequeued1)
      self.assertEqual(result0, result1)
      self.assertEqual(1, _count_ipu_compilations(report0))
      self.assertEqual(0, _count_ipu_compilations(report1))

  @test_util.deprecated_graph_mode_only
  def test_model_with_send_to_host_op(self):
    def build_and_run_model():
      def my_net(x):
        return gen_sendrecv_ops.ipu_send_to_host(x,
                                                 tensor_name="test_tensor",
                                                 send_device="/device:IPU:0",
                                                 send_device_incarnation=0,
                                                 recv_device="/device:CPU:0")

      v = array_ops.placeholder(np.float32, shape=())
      with ipu.scopes.ipu_scope("/device:IPU:0"):
        send_op = ipu.ipu_compiler.compile(my_net, inputs=[v])
      with ops.device("/device:CPU:0"):
        recv_op = gen_sendrecv_ops.ipu_recv_at_host(
            T=np.float32,
            tensor_name="test_tensor",
            send_device="/device:IPU:0",
            send_device_incarnation=0,
            recv_device="/device:CPU:0")

      compile_report = ipu_compile_summary("compile_report", send_op)

      tu.configure_ipu_system()
      with session.Session() as sess:
        return sess.run([send_op, recv_op, compile_report], feed_dict={v: 1.0})

    with _temporary_executable_cache():
      _, received0, report0 = _run_in_new_process(build_and_run_model)
      _, received1, report1 = _run_in_new_process(build_and_run_model)
      self.assertEqual(received0, received1)
      self.assertEqual(received0, 1.0)
      self.assertEqual(1, _count_ipu_compilations(report0))
      self.assertEqual(0, _count_ipu_compilations(report1))

  @test_util.deprecated_graph_mode_only
  def test_new_graph_in_same_process(self):
    def build_and_run_model():
      def my_net(x):
        return x * x

      v = array_ops.placeholder(dtype=np.float32, shape=())
      with ipu.scopes.ipu_scope("/device:IPU:0"):
        [result] = ipu.ipu_compiler.compile(my_net, inputs=[v])
      compile_report = ipu_compile_summary("compile_report", result)

      tu.configure_ipu_system()
      with session.Session() as sess:
        return sess.run([result, compile_report], feed_dict={v: 2.0})

    with _temporary_executable_cache():
      # Since each Graph will have its own XLA compilation cache,
      # the cache we test is the last-level Poplar executable cache.

      with ops.Graph().as_default():
        result0, report0 = build_and_run_model()

      with ops.Graph().as_default():
        result1, report1 = build_and_run_model()

      self.assertEqual(result0, result1)
      self.assertEqual(1, _count_ipu_compilations(report0))
      self.assertEqual(0, _count_ipu_compilations(report1))

  def test_ipu_estimator(self):
    def my_model_fn(features, labels, mode):
      loss = features + labels
      # Make different graphs for train and eval
      if mode == model_fn.ModeKeys.TRAIN:
        train_op = array_ops.identity(loss)
        return model_fn.EstimatorSpec(mode=mode, loss=loss, train_op=train_op)
      elif mode == model_fn.ModeKeys.EVAL:
        eval_metric_ops = {"metric": metrics_impl.mean(features * labels)}
        return model_fn.EstimatorSpec(mode=mode,
                                      loss=loss,
                                      eval_metric_ops=eval_metric_ops)
      else:
        raise NotImplementedError(mode)

    def my_input_fn():
      dataset = dataset_ops.Dataset.from_tensor_slices((
          [[0], [1]],
          [[2], [3]],
      ))
      return dataset.batch(1, drop_remainder=True)

    def build_and_run_model():
      ipu_options = ipu.utils.create_ipu_config(profiling=True)

      ipu_config = ipu.ipu_run_config.IPURunConfig(iterations_per_loop=2,
                                                   ipu_options=ipu_options,
                                                   compile_summary=True)

      run_config = ipu.ipu_run_config.RunConfig(ipu_run_config=ipu_config)
      estimator = ipu.ipu_estimator.IPUEstimator(model_fn=my_model_fn,
                                                 config=run_config)

      log_dir = estimator.model_dir
      self.assertEqual(0, _count_ipu_compilations_in_dir(log_dir))

      # Compile the training graph
      estimator.train(input_fn=my_input_fn, steps=2)
      self.assertEqual(1, _count_ipu_compilations_in_dir(log_dir))

      # Re-use cached training graph
      estimator.train(input_fn=my_input_fn, steps=2)
      self.assertEqual(1, _count_ipu_compilations_in_dir(log_dir))

      # Compile the evaluation graph
      estimator.evaluate(input_fn=my_input_fn, steps=2)
      self.assertEqual(2, _count_ipu_compilations_in_dir(log_dir))

      # Re-use cached evaluation graph
      estimator.evaluate(input_fn=my_input_fn, steps=2)
      self.assertEqual(2, _count_ipu_compilations_in_dir(log_dir))

      # Re-use cached training graph
      estimator.train(input_fn=my_input_fn, steps=2)
      self.assertEqual(2, _count_ipu_compilations_in_dir(log_dir))

    with _temporary_executable_cache():
      _run_in_new_process(build_and_run_model)


if __name__ == "__main__":
  googletest.main()
