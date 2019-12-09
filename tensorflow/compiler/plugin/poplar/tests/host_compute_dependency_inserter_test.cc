/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/plugin/poplar/driver/passes/host_compute_dependency_inserter.h"

#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace poplarplugin {
namespace {

using HostComputeDependencyInserterTest = HloTestBase;

TEST_F(HostComputeDependencyInserterTest, TestInsertOneDependency) {
  std::string hlo_string = R"(
HloModule top

ENTRY %top (arg: f32[]) -> f32[] {
  %arg = f32[] parameter(0), parameter_replication={false}, metadata={op_name="XLA_Args"}
  %send-token = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send = (f32[], u32[], token[]) send(f32[] %arg, token[] %send-token), channel_id=1, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send-done = token[] send-done((f32[], u32[], token[]) %send), channel_id=1, is_host_transfer=true, frontend_attributes={rendezvous_key=send_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-token = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv = (f32[], u32[], token[]) recv(token[] %recv-token), channel_id=2, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-done = (f32[], token[]) recv-done((f32[], u32[], token[]) %recv), channel_id=2, is_host_transfer=true, frontend_attributes={rendezvous_key=recv_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  ROOT %get-tuple-element = f32[] get-tuple-element((f32[], token[]) %recv-done), index=0, metadata={op_type="XlaHostCompute" op_name="host_compute"}
}
)";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module_or_status = ParseAndReturnVerifiedModule(hlo_string, config);
  EXPECT_TRUE(module_or_status.ok());

  auto* module = module_or_status.ValueOrDie().get();
  auto* comp = module->entry_computation();

  HostComputeDependencyInserter inserter;
  ASSERT_TRUE(inserter.Run(module).ValueOrDie());

  auto* send = comp->GetInstructionWithName("send-done");
  ASSERT_NE(send, nullptr);
  auto* recv = comp->GetInstructionWithName("recv-done");
  ASSERT_NE(recv, nullptr);
  ASSERT_EQ(recv->control_predecessors().size(), 1);
  ASSERT_EQ(recv->control_predecessors()[0], send);
}

TEST_F(HostComputeDependencyInserterTest, TestNoDependencyBetweenDifferentOps) {
  std::string hlo_string = R"(
HloModule top

ENTRY %top (arg: f32[]) -> f32[] {
  %arg = f32[] parameter(0), parameter_replication={false}, metadata={op_name="XLA_Args"}
  %send-token = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send = (f32[], u32[], token[]) send(f32[] %arg, token[] %send-token), channel_id=1, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send-done = token[] send-done((f32[], u32[], token[]) %send), channel_id=1, is_host_transfer=true, frontend_attributes={rendezvous_key=send_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-token = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute_2"}
  %recv = (f32[], u32[], token[]) recv(token[] %recv-token), channel_id=2, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute_2"}
  %recv-done = (f32[], token[]) recv-done((f32[], u32[], token[]) %recv), channel_id=2, is_host_transfer=true, frontend_attributes={rendezvous_key=recv_key}, metadata={op_type="XlaHostCompute" op_name="host_compute_2"}
  ROOT %get-tuple-element = f32[] get-tuple-element((f32[], token[]) %recv-done), index=0, metadata={op_type="XlaHostCompute" op_name="host_compute"}
}
)";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module_or_status = ParseAndReturnVerifiedModule(hlo_string, config);
  EXPECT_TRUE(module_or_status.ok());

  auto* module = module_or_status.ValueOrDie().get();
  auto* comp = module->entry_computation();

  HostComputeDependencyInserter inserter;
  ASSERT_TRUE(inserter.Run(module).ValueOrDie());

  auto* send = comp->GetInstructionWithName("send-done");
  ASSERT_NE(send, nullptr);
  auto* recv = comp->GetInstructionWithName("recv-done");
  ASSERT_NE(recv, nullptr);
  ASSERT_EQ(recv->control_predecessors().size(), 0);
}

TEST_F(HostComputeDependencyInserterTest,
       TestInsertDependenciesFromAllSendsToAllRecvs) {
  std::string hlo_string = R"(
HloModule top

ENTRY %top (arg: f32[]) -> f32[] {
  %arg = f32[] parameter(0), parameter_replication={false}, metadata={op_name="XLA_Args"}
  %send-token.1 = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send.1 = (f32[], u32[], token[]) send(f32[] %arg, token[] %send-token.1), channel_id=1, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send-done.1 = token[] send-done((f32[], u32[], token[]) %send.1), channel_id=1, is_host_transfer=true, frontend_attributes={rendezvous_key=send_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send-token.2 = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send.2 = (f32[], u32[], token[]) send(f32[] %arg, token[] %send-token.2), channel_id=2, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %send-done.2 = token[] send-done((f32[], u32[], token[]) %send.2), channel_id=2, is_host_transfer=true, frontend_attributes={rendezvous_key=send_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-token.1 = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv.1 = (f32[], u32[], token[]) recv(token[] %recv-token.1), channel_id=3, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-done.1 = (f32[], token[]) recv-done((f32[], u32[], token[]) %recv.1), channel_id=3, is_host_transfer=true, frontend_attributes={rendezvous_key=recv_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-token.2 = token[] after-all(), metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv.2 = (f32[], u32[], token[]) recv(token[] %recv-token.2), channel_id=4, is_host_transfer=true, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  %recv-done.2 = (f32[], token[]) recv-done((f32[], u32[], token[]) %recv.2), channel_id=4, is_host_transfer=true, frontend_attributes={rendezvous_key=recv_key}, metadata={op_type="XlaHostCompute" op_name="host_compute"}
  ROOT %get-tuple-element = f32[] get-tuple-element((f32[], token[]) %recv-done.2), index=0, metadata={op_type="XlaHostCompute" op_name="host_compute"}
}
)";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module_or_status = ParseAndReturnVerifiedModule(hlo_string, config);
  EXPECT_TRUE(module_or_status.ok());

  auto* module = module_or_status.ValueOrDie().get();
  auto* comp = module->entry_computation();

  HostComputeDependencyInserter inserter;
  ASSERT_TRUE(inserter.Run(module).ValueOrDie());

  auto* send1 = comp->GetInstructionWithName("send-done.1");
  ASSERT_NE(send1, nullptr);
  auto* send2 = comp->GetInstructionWithName("send-done.2");
  ASSERT_NE(send2, nullptr);

  auto* recv1 = comp->GetInstructionWithName("recv-done.1");
  ASSERT_NE(recv1, nullptr);
  ASSERT_EQ(recv1->control_predecessors().size(), 2);
  ASSERT_EQ(recv1->control_predecessors()[0], send1);
  ASSERT_EQ(recv1->control_predecessors()[1], send2);

  auto* recv2 = comp->GetInstructionWithName("recv-done.2");
  ASSERT_NE(recv2, nullptr);
  ASSERT_EQ(recv2->control_predecessors().size(), 2);
  ASSERT_EQ(recv2->control_predecessors()[0], send1);
  ASSERT_EQ(recv2->control_predecessors()[1], send2);
}

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
