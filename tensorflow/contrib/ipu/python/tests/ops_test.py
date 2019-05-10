from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import tempfile

from tensorflow.compiler.plugin.poplar.driver.config_pb2 import IpuOptions
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.contrib.ipu import ipu_compiler
from tensorflow.keras import layers
from tensorflow.python.client import session as sl
from tensorflow.python.framework import test_util
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import rnn
from tensorflow.python.ops import rnn_cell
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.ops.losses import losses
from tensorflow.python.platform import googletest
from tensorflow.contrib import ipu
from tensorflow.python.training import gradient_descent


def count_compile_end_events(events):
  fn = (lambda x: 1 if x.type == IpuTraceEvent.COMPILE_END and len(
      x.compile_end.compilation_report) > 10 else 0)
  return sum(map(fn, events))


class ContribIpuOpsTest(test_util.TensorFlowTestCase):
  def testSummary(self):
    with ops.device("/device:IPU:0"):
      a = array_ops.placeholder(np.float32, [1], name="a")
      b = array_ops.placeholder(np.float32, [1], name="b")
      out = a + b

    summary = ipu.ops.ipu_compile_summary('comp', out)

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      fd = {
          a: [1.0],
          b: [2.0],
      }
      result, s = sess.run([out, summary], fd)
      self.assertAllClose(result, [3.0])
      self.assertTrue(len(s) > 100)

  def testCreateConfig(self):
    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.auto_select_ipus(cfg, [1, 1])
    self.assertTrue(isinstance(cfg, IpuOptions))
    self.assertTrue(len(cfg.device_config), 2)

    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.auto_select_ipus(cfg, [4, 4])
    self.assertTrue(isinstance(cfg, IpuOptions))
    self.assertTrue(len(cfg.device_config), 2)
    self.assertTrue(cfg.device_config[0].auto_count, 4)
    self.assertTrue(cfg.device_config[1].auto_count, 4)

    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.auto_select_ipus(cfg, [4, 4], number_of_replicas=[2, 1])
    self.assertTrue(isinstance(cfg, IpuOptions))
    self.assertTrue(len(cfg.device_config), 2)
    self.assertTrue(cfg.device_config[0].auto_count, 4)
    self.assertTrue(cfg.device_config[0].num_replicas, 2)
    self.assertTrue(cfg.device_config[1].auto_count, 4)
    self.assertTrue(cfg.device_config[1].num_replicas, 1)

    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.select_ipus(cfg, [2, 3])
    self.assertTrue(isinstance(cfg, IpuOptions))
    self.assertTrue(len(cfg.device_config), 2)
    self.assertTrue(cfg.device_config[0].cfg_index, 2)
    self.assertTrue(cfg.device_config[1].cfg_index, 3)

    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.set_compilation_options(cfg, {'A': 'B', 'C': 'D'})
    self.assertTrue(len(cfg.compilation_options), 2)
    self.assertTrue(cfg.compilation_options[0].option, "A")
    self.assertTrue(cfg.compilation_options[0].value, "B")
    self.assertTrue(cfg.compilation_options[1].option, "C")
    self.assertTrue(cfg.compilation_options[1].value, "D")

    with self.assertRaises(Exception):
      cfg = ipu.utils.create_ipu_config()
      cfg = ipu.utils.auto_select_ipus(cfg, [4, 4])
      cfg = ipu.utils.select_ipus(cfg, [4, 4])

    with self.assertRaises(Exception):
      cfg = ipu.utils.create_ipu_config()
      cfg = ipu.utils.auto_select_ipus(cfg, [4, 4], number_of_replicas=1)

    with self.assertRaises(Exception):
      cfg = ipu.utils.create_ipu_config()
      cfg = ipu.utils.select_ipus(cfg, [4, 4], number_of_replicas=1)

    with self.assertRaises(Exception):
      cfg = ipu.utils.create_ipu_config(profiling=True, enable_ipu_events=True)

    with self.assertRaises(Exception):
      cfg = ipu.utils.create_ipu_config(
          profiling=False, profile_execution=True)

  def testEventFetchAndStringDecode(self):
    with ops.device("/device:IPU:0"):
      a = array_ops.placeholder(np.float32, [1], name="a")
      b = array_ops.placeholder(np.float32, [1], name="b")
      out = a + b

    events = gen_ipu_ops.ipu_event_trace()

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      # Discard any existing events
      sess.run(events)

      fd = {
          a: [1.0],
          b: [2.0],
      }
      result = sess.run(out, fd)
      self.assertAllClose(result, [3.0])

      # 1x compile begin, 1x compile end, 1x load engine, 1x execute
      e = sess.run(events)
      self.assertEqual(len(e), 4)

      dump = ipu.utils.extract_all_strings_from_event_trace(e)
      self.assertTrue(len(dump) > 100)

  def testMaxReportSize(self):
    with ops.device("/device:IPU:0"):
      a = array_ops.placeholder(np.float32, [1], name="a")
      b = array_ops.placeholder(np.float32, [1], name="b")
      out = a + b

    events = gen_ipu_ops.ipu_event_trace()

    cfg = ipu.utils.create_ipu_config(
        profiling=True, profile_execution=True, max_report_size=10)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      # Discard any existing events
      sess.run(events)

      result = sess.run(out, {a: [1.0], b: [2.0]})
      self.assertAllClose(result, [3.0])

      # 1x compile begin, 1x compile end, 1x load engine, 1x execute
      e = sess.run(events)
      self.assertEqual(len(e), 4)

      reps = ipu.utils.extract_compile_reports(e)
      self.assertEqual(len(reps), 0)

      reps = ipu.utils.extract_execute_reports(e)
      self.assertEqual(len(reps), 0)

  def testDumpReportsToFile(self):
    with ops.device("/device:IPU:0"):
      a = array_ops.placeholder(np.float32, [1], name="a")
      b = array_ops.placeholder(np.float32, [1], name="b")
      out = a + b

    tmpdir = tempfile.mkdtemp()

    events = gen_ipu_ops.ipu_event_trace()

    cfg = ipu.utils.create_ipu_config(
        profiling=True, profile_execution=True, report_directory=tmpdir)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      # Discard any existing events
      sess.run(events)

      result = sess.run(out, {a: [1.0], b: [2.0]})
      self.assertAllClose(result, [3.0])

      # 1x compile begin, 1x compile end, 1x load engine, 1x execute
      e = sess.run(events)
      self.assertEqual(len(e), 4)

      reps = ipu.utils.extract_compile_reports(e)
      self.assertEqual(len(reps), 1)
      self.assertTrue(reps[0][1].startswith(tmpdir))
      with open(reps[0][1]) as f:
        rep = f.read()
        self.assertTrue(len(rep) > 1000)
        self.assertEqual(rep[0], '{')

      reps = ipu.utils.extract_execute_reports(e)
      self.assertEqual(len(reps), 1)
      self.assertTrue(reps[0][1].startswith(tmpdir))
      with open(reps[0][1]) as f:
        rep = f.read()
        self.assertTrue(len(rep) > 1000)
        self.assertEqual(rep[0], '{')

  def testIpuSimpleScopeAndExecutionReport(self):
    def my_net(a, b):
      c = a + b
      return [c]

    with ops.device('cpu'):
      a = array_ops.placeholder(np.float32, [1], name="a")
      b = array_ops.placeholder(np.float32, [1], name="b")
      events = gen_ipu_ops.ipu_event_trace()

    with ipu.ops.ipu_scope("/device:IPU:0"):
      r = ipu_compiler.compile(my_net, inputs=[a, b])

    cfg = ipu.utils.create_ipu_config(profiling=True, profile_execution=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:

      fd = {
          a: [1],
          b: [2],
      }

      sess.run(events)

      res = sess.run(r[0], fd)
      self.assertEqual(res, [3])

      e = sess.run(events)
      evts = ipu.utils.extract_all_events(e)
      self.assertEqual(count_compile_end_events(evts), 1)

      compilation_rep = ipu.utils.extract_compile_reports(e)
      self.assertEqual(len(compilation_rep), 1)
      self.assertEqual(type(compilation_rep), list)
      self.assertEqual(type(compilation_rep[0]), tuple)
      self.assertTrue(compilation_rep[0][0].startswith("cluster"))
      self.assertTrue(len(compilation_rep[0][1]) > 1000)
      self.assertTrue(compilation_rep[0][1].startswith('{'))

      execution_rep = ipu.utils.extract_execute_reports(e)
      self.assertEqual(len(execution_rep), 1)
      self.assertEqual(type(execution_rep), list)
      self.assertEqual(type(execution_rep[0]), tuple)
      self.assertTrue(execution_rep[0][0].startswith("cluster"))
      self.assertTrue(len(execution_rep[0][1]) > 1000)
      self.assertTrue(execution_rep[0][1].startswith('{'))

  def testIpuWhileScope(self):
    # 1: design is targetted at the device
    # 2: variables are resource variables
    # 3: training a while_loop is possible
    def my_net(a, b):
      c = variable_scope.get_variable('c', initializer=[1.0])
      self.assertTrue("ResourceVariable" in str(type(c)))

      lstm_cell = rnn_cell.LSTMCell(1, forget_bias=1.0)
      outputs, states = rnn.dynamic_rnn(lstm_cell, a, dtype=np.float32)

      logits = outputs[-1] * c
      self.assertEqual(logits.device, "/device:IPU:0")

      res = array_ops.reshape(logits, [1, 8, 1])

      l = losses.mean_squared_error(res, b)

      optimizer = gradient_descent.GradientDescentOptimizer(0.1)
      train = optimizer.minimize(l)

      return [l, train]

    with ops.device('cpu'):
      a = array_ops.placeholder(np.float32, [1, 8, 1], name="a")
      b = array_ops.placeholder(np.float32, [1, 8, 1], name="b")

    with ipu.ops.ipu_scope("/device:IPU:0"):

      l = ipu_compiler.compile(my_net, inputs=[a, b])

    cfg = ipu.utils.create_ipu_config()
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      # Initialize and then discard events relating to initialization
      sess.run(variables.global_variables_initializer())

      fd = {
          a: [[[1.], [1.], [1.], [1.], [1.], [1.], [1.], [1.]]],
          b: [[[1.], [1.], [1.], [1.], [1.], [1.], [1.], [1.]]],
      }

      l_initial = sess.run([l], fd)

      for _ in range(100):
        _ = sess.run([l], fd)

      l_final = sess.run([l], fd)

      self.assertTrue(l_initial > l_final)

  def testInitializerDeviceChange(self):

    inp = array_ops.placeholder(np.float32, [1, 8, 8, 4])
    with ipu.ops.ipu_scope("/device:IPU:0"):
      out = layers.Conv2D(8, 1)(inp)

    events = gen_ipu_ops.ipu_event_trace()

    ipu.utils.move_variable_initialization_to_cpu()

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      # Discard any pending events
      sess.run(events)

      # Run initializer (should be on CPU)
      sess.run(variables.global_variables_initializer())

      e = sess.run(events)
      self.assertEqual(len(e), 2)  # compile begin/end, no load/execute

  def testVarsInitializedByStreamsAreLoggedAsOnDevice(self):
    # This verifies that when an initialization graph has no ops in it (it is
    # a pass through of streaming inputs to initialized resources) then the
    # resources are logged as resources on the device so that a future execution
    # sees them as valid and on device
    w_val1 = np.array([1, 2, 3, 4])
    w_val2 = np.array([4, 3, 2, 1])
    w_val3 = np.array([9, 0, 9, 0])
    with ops.device("/device:IPU:1"):
      with variable_scope.variable_scope("vs", use_resource=True):
        w1 = variable_scope.get_variable(
            "w1",
            shape=[4],
            dtype=np.float32,
            initializer=init_ops.constant_initializer(
                w_val1, dtype=np.float32))
        w2 = variable_scope.get_variable(
            "w2",
            shape=[4],
            dtype=np.float32,
            initializer=init_ops.constant_initializer(
                w_val2, dtype=np.float32))
        w3 = variable_scope.get_variable(
            "w3",
            shape=[4],
            dtype=np.float32,
            initializer=init_ops.constant_initializer(
                w_val3, dtype=np.float32))

      y = w1 + w2 + w3

    ipu.utils.move_variable_initialization_to_cpu()

    cfg = ipu.utils.create_ipu_config(profiling=False)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    cfg = ipu.utils.auto_select_ipus(cfg, 2)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      sess.run(variables.global_variables_initializer())

      xs = [
          np.array([7, 3, 5, 9], dtype=np.float32),
          np.array([1, 8, 3, 4], dtype=np.float32),
          np.array([9, 2, 2, 6], dtype=np.float32)
      ]
      for x in xs:
        out = sess.run(y)
        self.assertAllClose(out, w_val1 + w_val2 + w_val3)

  def testMultiScopeTest(self):
    with ops.device('cpu'):
      x = array_ops.placeholder(np.float32, [2, 2])
      y = array_ops.placeholder(np.float32, [2, 2])
      report = gen_ipu_ops.ipu_event_trace()

    with ipu.ops.ipu_scope('/device:IPU:0'):
      z = math_ops.matmul(x, y)
    with ipu.ops.ipu_scope('/device:IPU:0'):
      z2 = math_ops.matmul(x, z)

    cfg = ipu.utils.create_ipu_config(profiling=True)
    cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
    ipu.utils.configure_ipu_system(cfg)

    with sl.Session() as sess:
      sess.run(report)
      result = sess.run(z2, {x: np.ones([2, 2]), y: np.ones([2, 2])})

      self.assertAllEqual(result, [[4, 4], [4, 4]])

      rep = sess.run(report)
      evts = ipu.utils.extract_all_types_from_event_trace(rep)

      num_compiles = 0
      num_executions = 0
      for e in evts:
        if e == IpuTraceEvent.COMPILE_END:
          num_compiles += 1
        if e == IpuTraceEvent.EXECUTE:
          num_executions += 1

      self.assertEqual(num_compiles, 1)
      self.assertEqual(num_executions, 1)


if __name__ == "__main__":
  googletest.main()