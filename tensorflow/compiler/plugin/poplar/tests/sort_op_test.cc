// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <poplar/IPUModel.hpp>
#include <popops/ElementWise.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>

#include "tensorflow/compiler/plugin/poplar/driver/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/xla/test.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>

using namespace poplar;
using namespace poplar::program;

namespace xla {
namespace poplarplugin {

static std::string GetPathToGraphProgFile(std::string filename) {
  Dl_info dlInfo;
  static const void* dummy;
  if (dladdr(&dummy, &dlInfo)) {
    std::string path(dlInfo.dli_fname);
    path = path.substr(0, path.find_last_of('/') + 1);
    path = path + "../compiler/plugin/poplar/" + filename;
    if (access(path.c_str(), R_OK) != -1) {
      return path;
    }
  }

  // This is for unit tests
  {
    char buf[256];
    getcwd(buf, 255);
    std::string path(buf);
    path = path + "/tensorflow/compiler/plugin/poplar/" + filename;
    if (access(path.c_str(), R_OK) != -1) {
      return path;
    }
  }

  return "";
}

template <typename T>
static std::vector<T> iota(std::size_t count) {
  std::vector<T> result(count);

  std::iota(result.begin(), result.end(), 0);

  return result;
}

template <typename T>
static std::vector<T> random(std::size_t count) {
  std::vector<T> result(count);

  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_int_distribution<int> uniform_dist(100, 999);
  std::generate(result.begin(), result.end(), std::bind(uniform_dist, e1));

  return result;
}

template <typename T>
static std::vector<T> zeros(std::size_t count) {
  std::vector<T> result(count);

  std::fill(result.begin(), result.end(), 0.0f);

  return result;
}

TEST(Sort, OneDimension) {
  IPUModel ipuModel;
  ipuModel.tilesPerIPU = 12;
  Device device = ipuModel.createDevice();
  Graph graph(device);
  popops::addCodelets(graph);
  graph.addCodelets(GetPathToGraphProgFile("heap_sort.gp"));

  const std::size_t tensor_size = 1024;

  Tensor a = graph.addVariable(FLOAT, {tensor_size}, "input");
  poputil::mapTensorLinearly(graph, a);
  graph.createHostWrite("a-write", a);
  graph.createHostRead("a-read", a);

  auto prog_status = CreateSort(graph, a, 0);
  ASSERT_TRUE(prog_status.ok());
  auto prog = prog_status.ValueOrDie();

  Engine engine(graph, prog);
  engine.load(device);

  const auto input_buffer = random<float>(tensor_size);
  engine.writeTensor("a-write", input_buffer.data());

  engine.run();

  std::vector<float> output_buffer = zeros<float>(tensor_size);
  engine.readTensor("a-read", output_buffer.data());

  EXPECT_TRUE(std::is_sorted(output_buffer.begin(), output_buffer.end()));
  EXPECT_TRUE(std::is_permutation(output_buffer.begin(), output_buffer.end(),
                                  input_buffer.begin()));
}

TEST(SortInt, OneDimension) {
  IPUModel ipuModel;
  ipuModel.tilesPerIPU = 12;
  Device device = ipuModel.createDevice();
  Graph graph(device);
  popops::addCodelets(graph);
  graph.addCodelets(GetPathToGraphProgFile("heap_sort.gp"));

  const std::size_t tensor_size = 1024;

  Tensor a = graph.addVariable(INT, {tensor_size}, "input");
  poputil::mapTensorLinearly(graph, a);
  graph.createHostWrite("a-write", a);
  graph.createHostRead("a-read", a);

  auto prog_status = CreateSort(graph, a, 0);
  ASSERT_TRUE(prog_status.ok());
  auto prog = prog_status.ValueOrDie();

  Engine engine(graph, prog);
  engine.load(device);

  const auto input_buffer = random<int>(tensor_size);
  engine.writeTensor("a-write", input_buffer.data());

  engine.run();

  auto output_buffer = zeros<int>(tensor_size);
  engine.readTensor("a-read", output_buffer.data());

  EXPECT_TRUE(std::is_sorted(output_buffer.begin(), output_buffer.end()));
  EXPECT_TRUE(std::is_permutation(output_buffer.begin(), output_buffer.end(),
                                  input_buffer.begin()));
}

TEST(SortKV, OneDimension) {
  IPUModel ipuModel;
  ipuModel.tilesPerIPU = 12;
  Device device = ipuModel.createDevice();
  Graph graph(device);
  popops::addCodelets(graph);
  graph.addCodelets(GetPathToGraphProgFile("heap_sort.gp"));

  const std::size_t tensor_size = 1024;

  Tensor k = graph.addVariable(FLOAT, {tensor_size}, "key");
  Tensor v = graph.addVariable(FLOAT, {tensor_size}, "value");
  poputil::mapTensorLinearly(graph, k);
  poputil::mapTensorLinearly(graph, v);
  graph.createHostWrite("a-write", k);
  graph.createHostWrite("b-write", v);
  graph.createHostRead("b-read", v);

  auto prog_status = CreateSort(graph, k, v, 0);
  ASSERT_TRUE(prog_status.ok());
  auto prog = prog_status.ValueOrDie();

  Engine engine(graph, prog);
  engine.load(device);

  auto input_buffer = iota<float>(tensor_size);
  std::reverse(input_buffer.begin(), input_buffer.end());
  engine.writeTensor("a-write", input_buffer.data());
  std::reverse(input_buffer.begin(), input_buffer.end());
  engine.writeTensor("b-write", input_buffer.data());

  engine.run();

  std::vector<float> output_buffer = zeros<float>(tensor_size);
  engine.readTensor("b-read", output_buffer.data());

  EXPECT_TRUE(std::is_sorted(output_buffer.rbegin(), output_buffer.rend()));
  EXPECT_TRUE(std::is_permutation(output_buffer.begin(), output_buffer.end(),
                                  input_buffer.begin()));
}

TEST(Sort, TwoDimension) {
  IPUModel ipuModel;
  ipuModel.tilesPerIPU = 12;
  Device device = ipuModel.createDevice();
  Graph graph(device);
  popops::addCodelets(graph);
  graph.addCodelets(GetPathToGraphProgFile("heap_sort.gp"));

  const std::size_t tensor_size = 32;

  Tensor a = graph.addVariable(FLOAT, {tensor_size, tensor_size}, "input");
  poputil::mapTensorLinearly(graph, a);
  graph.createHostWrite("a-write", a);
  graph.createHostRead("a-read", a);

  auto prog_status = CreateSort(graph, a, 1);
  ASSERT_TRUE(prog_status.ok());
  auto prog = prog_status.ValueOrDie();

  Engine engine(graph, prog);
  engine.load(device);

  const auto input_buffer = random<float>(tensor_size * tensor_size);
  engine.writeTensor("a-write", input_buffer.data());

  engine.run();

  std::vector<float> output_buffer = zeros<float>(tensor_size * tensor_size);
  engine.readTensor("a-read", output_buffer.data());
  for (int i = 0; i < tensor_size; ++i) {
    const auto begin_idx = i * tensor_size;
    const auto end_idx = begin_idx + tensor_size;

    const auto out_begin = std::next(output_buffer.begin(), begin_idx);
    const auto out_end = std::next(output_buffer.begin(), end_idx);
    const auto in_begin = std::next(input_buffer.begin(), begin_idx);

    EXPECT_TRUE(std::is_sorted(out_begin, out_end));
    EXPECT_TRUE(std::is_permutation(out_begin, out_end, in_begin));
  }
}

TEST(Sort, ThreeDimension) {
  IPUModel ipuModel;
  ipuModel.tilesPerIPU = 12;
  Device device = ipuModel.createDevice();
  Graph graph(device);
  popops::addCodelets(graph);
  graph.addCodelets(GetPathToGraphProgFile("heap_sort.gp"));

  const std::size_t tensor_size = 64;

  Tensor a =
      graph.addVariable(FLOAT, {tensor_size, tensor_size, tensor_size}, "key");
  poputil::mapTensorLinearly(graph, a);
  graph.createHostWrite("a-write", a);
  graph.createHostRead("a-read", a);

  auto prog_status = CreateSort(graph, a, 2);
  ASSERT_TRUE(prog_status.ok());
  auto prog = prog_status.ValueOrDie();

  Engine engine(graph, prog);
  engine.load(device);

  const auto input_buffer =
      random<float>(tensor_size * tensor_size * tensor_size);
  engine.writeTensor("a-write", input_buffer.data());

  engine.run();

  std::vector<float> output_buffer =
      zeros<float>(tensor_size * tensor_size * tensor_size);
  engine.readTensor("a-read", output_buffer.data());
  for (int i = 0; i < tensor_size; ++i) {
    for (int k = 0; k < tensor_size; ++k) {
      const auto begin_idx = i * tensor_size * tensor_size + k * tensor_size;
      const auto end_idx = begin_idx + tensor_size;

      const auto out_begin = std::next(output_buffer.begin(), begin_idx);
      const auto out_end = std::next(output_buffer.begin(), end_idx);
      const auto in_begin = std::next(input_buffer.begin(), begin_idx);

      EXPECT_TRUE(std::is_sorted(out_begin, out_end));
      EXPECT_TRUE(std::is_permutation(out_begin, out_end, in_begin));
    }
  }
}

}  // namespace poplarplugin
}  // namespace xla
