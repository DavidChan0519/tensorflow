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

#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"

namespace tensorflow {

REGISTER_OP("IpuUserOp")
    .Input("input: input_types")
    .Output("output: output_types")
    .Attr("input_types: list(type) >= 0")
    .Attr("output_types: list(type) >= 0")
    .Attr("output_shapes: list(shape) >= 0")
    .Attr("library_path: string")
    .Attr("gp_path: string")

    // We don't know what the user is going to do.
    .SetIsStateful()

    // Infer the shape from the output shapes list.
    .SetShapeFn([](shape_inference::InferenceContext* c) {
      std::vector<PartialTensorShape> shapes;
      TF_RETURN_IF_ERROR(c->GetAttr("output_shapes", &shapes));
      for (int i = 0; i < shapes.size(); ++i) {
        shape_inference::ShapeHandle out;
        TF_RETURN_IF_ERROR(c->MakeShapeFromPartialTensorShape(shapes[i], &out));
        c->set_output(i, out);
      }
      return Status::OK();
    })
    .Doc(R"doc(
        Adds a prebuilt user operation to the tensorflow graph. 
        input: The variadic input to the user op.
        output_shapes: The shape of each tuple element output
        output_types: The type of each tuple element output
        library_path: The path to the shared library containing
            the operation.
        gp_path (optional): Path to the gp file if provided.s
    )doc");

}  // namespace tensorflow
