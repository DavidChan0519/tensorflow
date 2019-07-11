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

import itertools
import networkx as nx
import numpy as np

from tensorflow.python.ops.variables import trainable_variables
from tensorflow.compiler.xla import xla_data_pb2
from tensorflow.core.framework import attr_value_pb2
from tensorflow.python.ipu import sharding
from tensorflow.python.platform import tf_logging as logging

prohibited_ops = frozenset(["NextIteration", "PopDatastreamInfeedDequeue"])


# This is a model for the size of a tensor on a tile.  It could be improved by
# accepting that tensors generate vertex state associated with the number of
# operations which consume them.
def tensor_memory_use(t):
  return t.shape.num_elements() * t.dtype.size


def children(op):
  return set(op for out in op.outputs for op in out.consumers())


def convert_ops_to_nx(fwd_ops, bwd_ops=None):
  grad_ops = [op for op in bwd_ops if 'gradients/' in op.name.lower()]
  bwd_inputs = [t for op in grad_ops for t in op.inputs]
  graph = nx.DiGraph()
  dictionary = dict()
  # collect all variables including momentum types
  variable_ops = [
      op for op in fwd_ops + bwd_ops
      if op.type == 'ReadVariableOp' and op.inputs[0].op.type == 'VarHandleOp'
  ]
  var_mem = dict()
  all_variables = [t.name for t in trainable_variables()]
  for var in all_variables:
    # assign all memory for momentum type variables to root trainable variable
    var_ops = [
        op for op in variable_ops
        if op.inputs[0].name.startswith(var.split(":")[0])
    ]
    var_mem[var] = np.sum(
        [tensor_memory_use(t) for op in var_ops for t in op.outputs])
  variables_seen = []
  for op in fwd_ops:
    if op.type == 'ReadVariableOp' \
            and op.inputs[0].op.type == 'VarHandleOp'\
            and op.inputs[0].name not in variables_seen\
            and op.inputs[0].name in all_variables:
      parameter_mem = var_mem[op.inputs[0].name]
      variables_seen.append(op.inputs[0].name)
    else:
      parameter_mem = 0
    bwd_links = [t for t in op.outputs if t in bwd_inputs]
    if bwd_links != [] and op.type != 'ReadVariableOp' and not (
        op.type == 'Cast' and list(op.inputs)[0].op.type == 'ReadVariableOp'):
      saved_mem = np.sum([tensor_memory_use(t) for t in bwd_links])
    else:
      saved_mem = 0
    bwd_links = {
        t.name: {
            'size': tensor_memory_use(t),
            'shape': t.shape.as_list()
        }
        for t in bwd_links
    }
    has_bwd_links = bwd_links != {}
    graph.add_node(
        op.name,
        bwd_links=bwd_links,
        saved_mem=saved_mem,
        has_bwd_links=has_bwd_links,
        parameter_mem=parameter_mem)
    dictionary[op.name] = op

  for op in fwd_ops:
    for c_op in children(op):
      if c_op in fwd_ops:
        graph.add_edges_from([(op.name, c_op.name)])

  return graph, dictionary


def calculate_memory(graph, nodes, parameter=True, saved=True):
  total_mem = 0
  for n in nodes:
    if saved:
      total_mem += graph.nodes[n]['saved_mem']
    if parameter:
      total_mem += graph.nodes[n]['parameter_mem']
  return total_mem


def set_ipu_shard(op, index):
  proto = xla_data_pb2.OpSharding(
      type=xla_data_pb2.OpSharding.MAXIMAL, tile_assignment_devices=[index])

  attr_value = attr_value_pb2.AttrValue(s=proto.SerializeToString())
  op._set_attr(sharding._XLA_SHARDING, attr_value)


def is_splitting_edge(G_fwd, edge, input_node, output_node):
  G = nx.DiGraph(G_fwd)
  G.remove_edge(edge[0], edge[1])
  W = list(nx.weakly_connected_components(G))
  if len(W) == 2:
    if not any([input_node in c and output_node in c for c in W]):
      return True
  return False


def find_all_subgraphs(graph, splitting_edges, input_node, output_node):
  graph = nx.DiGraph(graph)
  for edge in splitting_edges:
    graph.remove_edge(edge[0], edge[1])
  W = list(nx.weakly_connected_components(graph))

  subgraphs = []
  edges = []
  next_node = input_node
  while len(W) > 0:
    indices = (i for i, w in enumerate(W) if next_node in w)
    index = next(indices, None)
    assert index is not None, str(next_node) + " not in any subgraph"

    sg = W.pop(index)
    subgraphs += [graph.subgraph(sg)]
    if len(W) > 0:
      # find edge in subgraph
      edge_generator = (e for e in splitting_edges if e[0] in sg)
      edge = next(edge_generator, None)
      if edge is None:
        break
      next_node = edge[1]
      edges += [edge]

  assert output_node in sg, "output node must be in final subgraph"

  return subgraphs, edges


def automatic_sharding(num_shards, input_ts, loss_ts, edge_filter=None):
  """Automatically set shards for all connected nodes in graph.

  Args:
    :param num_shards: number of shards to split graph over
    :param input_ts: tensor closest to the datafeed in graph
    :param loss_ts: tensor closest to the loss in graph
    :param edge_filter: a callable predicate, with the signature fn(edge), where
                        edge is a tuple with the name of the source op, and the
                        name of the destination op.
  """

  loss_op = loss_ts.op

  all_ops = loss_op.graph.get_operations()
  op_list = list(filter(lambda o: 'IPU' in o.device, all_ops))

  fwd_ops = []

  assert len(op_list) > 0

  marked_collection = loss_op.graph.get_collection(sharding._IPU_AUTOSHARD)

  if len(marked_collection) > 0:
    fwd_ops = marked_collection
  else:
    for op in op_list:
      if not any([s in op.name.lower() for s in ['gradients/', '/update_']]):
        fwd_ops.append(op)

  fwd_ops = list(fwd_ops)
  bwd_ops = [o for o in op_list if o not in fwd_ops]

  fwd_ops = [o for o in fwd_ops if o.type not in prohibited_ops]

  if input_ts.op not in fwd_ops:
    input_op = [op for op in input_ts.consumers() if op in fwd_ops][0]
  else:
    input_op = input_ts

  input_name = input_op.name.split(':')[0]

  graph, dictionary = convert_ops_to_nx(fwd_ops, bwd_ops)

  # check graph is a single weakly connected component
  # if not find the component with the loss op in and use that
  W = list(nx.weakly_connected_components(graph))
  if len(W) > 1:
    for g in W:
      if loss_op.name in g:
        graph = graph.subgraph(g)

    fwd_ops = [op for op in fwd_ops if op.name in graph.nodes]

  if nx.number_weakly_connected_components(graph) != 1:
    logging.fatal('Error: number of disconnected subgraphs in autosharder is '
                  + str(nx.number_weakly_connected_components(graph)))
    assert False

  graph_fwd = graph.subgraph([op.name for op in fwd_ops])

  # find all graph edges that split the graph into two subgraphs where the input
  # and output are not in the same subgraph
  splitting_edges = [
      edge for edge in graph_fwd.edges
      if is_splitting_edge(graph_fwd, edge, input_name, loss_op.name)
  ]

  if edge_filter and callable(edge_filter):
    splitting_edges = list(
        filter(lambda e: not edge_filter(e), splitting_edges))

  logging.debug('Possible splitting edges ' + str(splitting_edges))

  # given the splitting edges found find all of the subgraphs created and order
  # them
  subgraphs, edges = find_all_subgraphs(graph_fwd, splitting_edges, input_name,
                                        loss_op.name)

  subgraph_mem = [calculate_memory(graph_fwd, g) for g in subgraphs]
  logging.debug('Subgraph memory use ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g)) / (1024 * 1024))
      for g in subgraphs
  ]))

  logging.debug('Subgraph memory use (variables only) ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g, saved=False)) / (1024 * 1024))
      for g in subgraphs
  ]))

  logging.debug('Subgraph memory use (activations only) ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g, parameter=False)) /
          (1024 * 1024)) for g in subgraphs
  ]))

  # Verify that we have enough subgraphs to fill all of the available shards
  if len(edges) + 1 < num_shards:
    raise Exception(
        "There are fewer subgraphs (%s) than available shards (%s). Reduce the "
        "number of shards." % (len(edges) + 1, num_shards))

  # Split the ordered subgraphs into n groups and calculate the memory for each
  # possible combination
  #
  # Choose the best grouping based on:
  #       1. min max memory
  #       2. variance of memory
  # could use minimum data transferred between IPUs?
  min_max_mem = np.inf
  best_ind = []
  best_mem = []

  for ind in itertools.combinations(range(len(edges)), num_shards - 1):
    ind_pad = [0] + [i + 1 for i in ind] + [len(subgraph_mem)]
    mem = [
        np.sum(subgraph_mem[ind_pad[i]:ind_pad[i + 1]])
        for i in range(len(ind) + 1)
    ]
    max_mem = np.max(mem)
    if max_mem < min_max_mem:
      best_ind = [ind]
      best_mem = [mem]
      min_max_mem = max_mem
    elif max_mem == min_max_mem:
      best_ind += [ind]
      best_mem += [mem]

  min_var = np.inf
  for ind, mem in zip(best_ind, best_mem):
    var_mem = np.var(mem)
    if var_mem < min_var:
      best_ind = [ind]
      best_mem = [mem]
      min_var = var_mem
    elif var_mem == min_var:
      best_ind += [ind]
      best_mem += [mem]

  # if still tied choose the first option in the list
  best_ind = best_ind[0]

  logging.debug('Splitting edges ' +
                str(list(map(lambda x: str(splitting_edges[x]), best_ind))))

  ind_pad = [0] + [i + 1 for i in best_ind] + [len(subgraph_mem)]
  per_shard_subgraphs = [
      graph_fwd.subgraph(
          [g0 for g in subgraphs[ind_pad[i]:ind_pad[i + 1]] for g0 in g.nodes])
      for i in range(len(ind) + 1)
  ]

  logging.debug('Per shard subgraph memory use ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g)) / (1024 * 1024))
      for g in per_shard_subgraphs
  ]))
  logging.debug('Per shard subgraph memory use (variables only) ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g, saved=False)) / (1024 * 1024))
      for g in per_shard_subgraphs
  ]))
  logging.debug('Per shard subgraph memory use (activations only) ' + str([
      "{:.4g} MiB".format(
          float(calculate_memory(graph_fwd, g, parameter=False)) /
          (1024 * 1024)) for g in per_shard_subgraphs
  ]))

  for op in fwd_ops:
    shard_set = False
    for i, g in enumerate(per_shard_subgraphs):
      if op.name in g:
        set_ipu_shard(op, i)
        shard_set = True
    assert shard_set, "%s not in any graph split" % op.name

  for op in filter(lambda o: o not in fwd_ops, op_list):
    attr = sharding.get_shard_from_colocation(op)
    if not attr:
      for child in children(op):
        attr = sharding.get_shard_from_colocation(child)

    if attr:
      op._set_attr(sharding._XLA_SHARDING, attr_value_pb2.AttrValue(s=attr))
