# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import contextlib
import fnmatch
import json as js
import numpy as np
import re

from tensorflow.compiler.plugin.poplar.driver.config_pb2 import IpuOptions
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.compiler.xla import xla_data_pb2
from tensorflow.core.framework import summary_pb2
from tensorflow.core.framework import attr_value_pb2
from tensorflow.core.protobuf import config_pb2
from tensorflow.python.data.ops.dataset_ops import Dataset
from tensorflow.python.client import session as session_lib
from tensorflow.python.framework import ops
from tensorflow.python.ops import gen_array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.summary.summary import tensor_summary


def configure_ipu_system(compilation_trace=True,
                         io_trace=False,
                         execution_trace=True,
                         report_every_nth_execution=0,
                         text_report=True,
                         cbor_report=False,
                         sharded=False,
                         replicated=False,
                         compile_ipu_code=False,
                         enable_ipu_events=False,
                         engine_opts=None):
  opts = IpuOptions()
  opts.profiling.enable_ipu_trace_events = (compilation_trace or io_trace
                                            or execution_trace
                                            or enable_ipu_events)
  opts.profiling.enable_compilation_trace = compilation_trace
  opts.profiling.enable_io_trace = io_trace
  opts.profiling.enable_execution_trace = execution_trace
  opts.profiling.enable_poplar_reports_text = text_report
  opts.profiling.enable_poplar_reports_cbor = cbor_report
  opts.profiling.report_every_nth_execution = report_every_nth_execution
  opts.profiling.max_report_size = 0x10000000  # 256MB
  opts.ipu_model_config.enable_ipu_model = True
  opts.ipu_model_config.compile_ipu_code = compile_ipu_code

  if engine_opts:
    for o in engine_opts.items():
      opt = opts.compilation_options.add()
      opt.option = o[0]
      opt.value = o[1]

  # When sharded we use two devices.
  device_count = 2 if sharded else 0

  if replicated:
    # When replicating, we either double the number of IPUs, or if it was 0, we create 2 replicas.
    device_count = device_count * 2 if device_count else 2

  if device_count:
    dev = opts.device_config.add()
    dev.auto_count = device_count

  g = ops.Graph()
  with g.as_default():
    cfg_op = gen_ipu_ops.ipu_configure_hardware(opts.SerializeToString())

  with session_lib.Session(graph=g) as sess:
    sess.run(cfg_op)


@contextlib.contextmanager
def ipu_session():
  with session_lib.Session() as sess:
    yield sess


def get_total_memory_from_report(report):
  lines = report.split('\n')
  found = False
  for l in lines:
    if not found:
      m = re.search('Memory Usage:', l)
      if m:
        found = True
    else:
      m = re.search('Including Gaps: +([\d,]+) B', l)
      if m:
        return int(m.group(1).replace(',', ''))
  return None


def get_compute_sets_from_report(report):
  lines = report.split('\n')
  cs = [x for x in lines if re.search(' OnTileExecute .*: ', x)]
  cs = [x.split(":")[1].strip() for x in cs]
  cs = [x.split()[0] for x in cs]
  return cs


def get_maximum_tile_size_from_events(report):
  lines = report.split('\n')
  found = False
  for l in lines:
    if not found:
      m = re.search('Memory Usage:', l)
      if m:
        found = True
    else:
      m = re.match(r' +Maximum.*: ([\d,]+) .*on tile [\d,]+', l)
      if m:
        return int(m.group(1).replace(',', ''))
  return None


def get_always_live_size_from_events(report):
  lines = report.split('\n')
  found = False
  for l in lines:
    m = re.match(r' +Always-live bytes: ([\d,]+)', l)
    if m:
      return int(m.group(1).replace(',', ''))
  return None


def check_compute_sets_not_in_blacklist(cs_list, bl):
  result = True
  fail_list = []
  for x in bl:
    matches = [cs for cs in cs_list if fnmatch.fnmatch(cs, x)]
    if len(matches) > 0:
      fail_list += matches
      result = False
  if not result:
    print("Compute sets present: " + str(fail_list))
  return result


def check_whitelist_entries_in_compute_sets(cs_list, whitelist):
  result = True
  fail_list = []
  wl = [x + '*' for x in whitelist]
  for cs in cs_list:
    if len([x for x in wl if fnmatch.fnmatch(cs, x)]) == 0:
      fail_list += [cs]
      result = False
  if not result:
    print("Failed to match " + str(fail_list))
  return result


def check_compute_sets_in_whitelist_entries(cs_list, whitelist):
  result = True
  fail_list = []
  wl = [x + '*' for x in whitelist]
  for x in wl:
    if len([cs for cs in cs_list if fnmatch.fnmatch(cs, x)]) == 0:
      fail_list += [x]
      result = False
  if not result:
    print("Failed to match " + str(fail_list))
  return result


def check_all_compute_sets_and_list(cs_list, whitelist):
  return (check_whitelist_entries_in_compute_sets(cs_list, whitelist)
          and check_compute_sets_in_whitelist_entries(cs_list, whitelist))


def count_compute_sets_matching(cs_list, to_match):
  cs_set = set(cs_list)
  return len([cs for cs in cs_set if fnmatch.fnmatch(cs, to_match)])


class ReportJSON:
  def __init__(self, test, events):
    self.test = test
    self.report = {}
    for e in events:
      evt = IpuTraceEvent.FromString(e)
      try:
        if evt.type == IpuTraceEvent.COMPILE_BEGIN:
          pass
        if evt.type == IpuTraceEvent.COMPILE_END:
          if evt.compile_end.compilation_report:
            self.report[IpuTraceEvent.COMPILE_END] = js.loads(
                evt.compile_end.compilation_report, encoding="utf-8")
        if evt.type == IpuTraceEvent.HOST_TO_DEVICE_TRANSFER:
          if evt.data_transfer.data_transfer:
            self.report[IpuTraceEvent.HOST_TO_DEVICE_TRANSFER] = js.loads(
                evt.data_transfer.data_transfer, encoding="utf-8")
        if evt.type == IpuTraceEvent.DEVICE_TO_HOST_TRANSFER:
          if evt.data_transfer.data_transfer:
            self.report[IpuTraceEvent.DEVICE_TO_HOST_TRANSFER] = js.loads(
                evt.data_transfer.data_transfer, encoding="utf-8")
        if evt.type == IpuTraceEvent.LOAD_ENGINE:
          pass
        if evt.type == IpuTraceEvent.EXECUTE:
          if evt.execute.execution_report:
            self.report[IpuTraceEvent.EXECUTE] = js.loads(
                evt.execute.execution_report, encoding="utf-8")
      except UnicodeDecodeError:
        pass

  # Excluding gaps
  def get_max_tile_size(self):
    return max(
        self.report[IpuTraceEvent.COMPILE_END]["memory"]["byTile"]["total"])

  def get_compute_sets(self):
    return self.report[IpuTraceEvent.COMPILE_END]["computeSets"]["names"]

  def assert_max_tile_size_in_range(self, low, high):
    self.test.assertAllInRange([self.get_max_tile_size()], low, high)

  def assert_all_compute_sets_and_list(self, ok):
    self.test.assertTrue(
        check_all_compute_sets_and_list(self.get_compute_sets(), ok))


def extract_all_strings_from_event_trace(events):
  result = ""
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    try:
      if evt.type == IpuTraceEvent.COMPILE_BEGIN:
        pass
      if evt.type == IpuTraceEvent.COMPILE_END:
        result = result + evt.compile_end.compilation_report.decode('utf-8')
      if evt.type == IpuTraceEvent.HOST_TO_DEVICE_TRANSFER:
        result = result + evt.data_transfer.data_transfer.decode('utf-8')
      if evt.type == IpuTraceEvent.DEVICE_TO_HOST_TRANSFER:
        result = result + evt.data_transfer.data_transfer.decode('utf-8')
      if evt.type == IpuTraceEvent.LOAD_ENGINE:
        pass
      if evt.type == IpuTraceEvent.EXECUTE:
        result = result + evt.execute.execution_report.decode('utf-8')
    except UnicodeDecodeError:
      pass
  return result


def get_compute_sets_from_json_report(event):
  if event.type == IpuTraceEvent.COMPILE_END:
    rep = js.loads(event.compile_end.compilation_report.decode('utf-8'))
    return rep['computeSets']['names']
  else:
    return []


def get_all_global_exchange_from_json_report(event):
  if event.type == IpuTraceEvent.COMPILE_END:
    rep = js.loads(event.compile_end.compilation_report.decode('utf-8'))
    return [
        p['name'] for p in rep['programs'] if p['type'] == 'GlobalExchange'
    ]
  else:
    return []


def extract_all_types_from_event_trace(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    result += [evt.type]
  return result


def extract_all_events(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    result += [evt]
  return result


def extract_all_execute_events(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    if evt.type == IpuTraceEvent.EXECUTE:
      result += [evt]
  return result


def extract_all_io_events(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    if evt.type in [
        IpuTraceEvent.HOST_TO_DEVICE_TRANSFER,
        IpuTraceEvent.DEVICE_TO_HOST_TRANSFER
    ]:
      try:
        payload = js.loads(evt.data_transfer.data_transfer.decode('utf-8'))
        for t in payload["tensors"]:
          result += [(evt.type, t["name"])]
      except UnicodeDecodeError:
        pass
  return result


def create_multi_increasing_dataset(value,
                                    shapes=[[1, 32, 32, 4], [1, 8]],
                                    dtypes=[np.float32, np.float32],
                                    repeat=True):
  def _get_one_input(data):
    result = []
    for i in range(len(shapes)):
      result.append(
          math_ops.cast(
              gen_array_ops.broadcast_to(data, shape=shapes[i]),
              dtype=dtypes[i]))
    return result

  dataset = Dataset.range(value).map(_get_one_input)
  if repeat:
    dataset = dataset.repeat()
  return dataset


def create_dual_increasing_dataset(value,
                                   data_shape=[1, 32, 32, 4],
                                   label_shape=[1, 8],
                                   dtype=np.float32,
                                   repeat=True):
  return create_multi_increasing_dataset(
      value,
      shapes=[data_shape, label_shape],
      dtypes=[dtype, dtype],
      repeat=repeat)


def create_single_increasing_dataset(value,
                                     shape=[1, 32, 32, 4],
                                     dtype=np.float32,
                                     repeat=True):
  return create_multi_increasing_dataset(
      value, shapes=[shape], dtypes=[dtype], repeat=repeat)


def move_variable_initialization_to_cpu():
  graph = ops.get_default_graph()

  init_ops = []
  dep_ops = list(
      map(lambda x: x.initializer.inputs[1].op,
          graph.get_collection('variables')))
  visited = set()

  while len(dep_ops) > 0:
    op = dep_ops.pop()
    if not op in visited:
      visited.add(op)
      init_ops += [op]
      dep_ops += map(lambda x: x.op, op.inputs)

  for op in init_ops:
    op._set_device('/device:CPU:0')
    op._set_attr('_class', attr_value_pb2.AttrValue(s=b'loc:@cpu'))
    op._set_attr('_XlaCompile', attr_value_pb2.AttrValue(b=False))
    op._set_attr('_XlaScope', attr_value_pb2.AttrValue(s=b''))
