/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/data_layout_transform.h"
#include "paddle/fluid/framework/feed_fetch_type.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/platform/profiler.h"

namespace paddle {
namespace operators {

class FetchOp : public framework::OperatorBase {
 public:
  FetchOp(const std::string &type, const framework::VariableNameMap &inputs,
          const framework::VariableNameMap &outputs,
          const framework::AttributeMap &attrs)
      : OperatorBase(type, inputs, outputs, attrs) {}

 private:
  void RunImpl(const framework::Scope &scope,
               const platform::Place &place) const override {
    OP_INOUT_CHECK(HasInputs("X"), "Input", "X", "Fetch");
    OP_INOUT_CHECK(HasOutputs("Out"), "Output", "Out", "Fetch");

    auto fetch_var_name = Input("X");
    auto *fetch_var = scope.FindVar(fetch_var_name);
    PADDLE_ENFORCE_NOT_NULL(
        fetch_var,
        platform::errors::NotFound(
            "Input variable(%s) cannot be found in scope for operator 'Fetch'.",
            fetch_var_name));

    auto out_name = Output("Out");
    auto *out_var = scope.FindVar(out_name);
    PADDLE_ENFORCE_NOT_NULL(out_var, platform::errors::NotFound(
                                         "Output variable(%s) cannot be found "
                                         "in scope for operator 'Fetch'.",
                                         out_name));

    int col = Attr<int>("col");
    PADDLE_ENFORCE_GE(
        col, 0, platform::errors::InvalidArgument(
                    "Expected the column index (the attribute 'col' of "
                    "operator 'Fetch') of current fetching variable to be "
                    "no less than 0. But received column index = %d.",
                    col));

    VLOG(3) << "Fetch variable " << fetch_var_name << " to variable "
            << out_name << "'s " << col << " column.";

    auto *fetch_list = out_var->GetMutable<framework::FeedFetchList>();
    auto &src_item = fetch_var->Get<framework::FeedFetchType>();

    if (static_cast<size_t>(col) >= fetch_list->size()) {
      fetch_list->resize(col + 1);
    }
    auto &dst_item = fetch_list->at(col);

    // FIXME(yuyang18): Should we assume the fetch operator always generate
    // CPU outputs?
    if (src_item.IsInitialized() && src_item.numel() > 0) {
#ifdef PADDLE_WITH_MKLDNN
      // Conversion from MKL-DNN to Paddle
      if (src_item.layout() == framework::DataLayout::kMKLDNN) {
        framework::Tensor out;
        // Convert to desired Paddle layout, apart from grads of filter
        // as params are not a subject to paddle's data_format
        framework::innerTransDataLayoutFromMKLDNN(
            src_item.layout(),
            fetch_var_name == framework::GradVarName("Filter")
                ? framework::DataLayout::kNCHW
                : paddle::platform::get_cur_paddle_data_layout(),
            src_item, &out, platform::CPUPlace());
        TensorCopySync(out, platform::CPUPlace(), &dst_item);
      } else {
        TensorCopySync(src_item, platform::CPUPlace(), &dst_item);
      }
#else
      TensorCopySync(src_item, platform::CPUPlace(), &dst_item);
#endif
    } else {
      // Not copy, if the src tensor is empty.
      dst_item.clear();
      dst_item.Resize({0});
    }
    dst_item.set_lod(src_item.lod());
  }
};

class FetchOpInfoMaker : public framework::OpProtoAndCheckerMaker {
 public:
  void Make() override {
    AddInput("X",
             "(LoDTensor) The resulted LoDTensor which is expected to return "
             "to users.");
    AddOutput("Out",
              "(vector<LoDTensor>) A fetching list of LoDTensor which may have "
              "different dimension, shape and data type.");
    AddAttr<int>("col", "(int) The column index of fetching object.");
    AddComment(R"DOC(
Fetch Operator.

It should not be configured by users directly.

)DOC");
  }
};
}  // namespace operators
}  // namespace paddle

REGISTER_OPERATOR(
    fetch, paddle::operators::FetchOp,
    paddle::framework::EmptyGradOpMaker<paddle::framework::OpDesc>,
    paddle::framework::EmptyGradOpMaker<paddle::imperative::OpBase>,
    paddle::operators::FetchOpInfoMaker);
