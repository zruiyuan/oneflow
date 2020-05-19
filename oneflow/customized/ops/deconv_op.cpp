#include "oneflow/core/framework/framework.h"
#include "oneflow/customized/ops/nn_util.h"

namespace oneflow {

namespace {

template<size_t NDims>
Maybe<void> InferTensorDesc4DeConv(user_op::InferContext* ctx) {
  const user_op::TensorDesc* in = ctx->TensorDesc4ArgNameAndIndex("in", 0);
  CHECK_EQ(NDims + 2, in->shape().NumAxes());

  const std::string& data_format = ctx->Attr<std::string>("data_format");
  const auto& kernel_size = ctx->Attr<std::vector<int32_t>>("kernel_size");
  CHECK_EQ_OR_RETURN(NDims, kernel_size.size());
  const int32_t filters = ctx->Attr<int32_t>("filters");
  size_t idx_offset = IdxOffset(data_format);

  // only support data parallel
  CHECK_OR_RETURN(ctx->parallel_ctx().parallel_num() == 1
                  || ctx->SbpParallel4ArgNameAndIndex("weight", 0).has_broadcast_parallel());

  {
    const auto& dilation_rate = ctx->Attr<std::vector<int32_t>>("dilation_rate");
    const auto& output_padding = ctx->Attr<std::vector<int32_t>>("output_padding");
    const auto& strides = ctx->Attr<std::vector<int32_t>>("strides");
    CHECK_EQ_OR_RETURN(NDims, dilation_rate.size());
    CHECK_EQ_OR_RETURN(NDims, strides.size());
    CHECK_EQ_OR_RETURN(NDims, output_padding.size());

    user_op::TensorDesc* out = ctx->TensorDesc4ArgNameAndIndex("out", 0);
    DimVector out_shape(NDims + 2);
    out_shape.at(0) = in->shape().At(0);
    const size_t c_dim = data_format == "channels_first" ? 1 : NDims + 1;
    out_shape.at(c_dim) = filters;
    for (int32_t i = 0; i < NDims; ++i) {
      CalcOutAndPadding4Deconv(in->shape().At(idx_offset + i), kernel_size.at(i),
                               dilation_rate.at(i), strides.at(i), output_padding.at(i),
                               static_cast<int32_t>(0), &out_shape.at(idx_offset + i), nullptr,
                               nullptr);
    }
    *out = *in;
    *out->mut_shape() = Shape(out_shape);
  }

  {
    DimVector weight_shape(in->shape().dim_vec());
    if (data_format == "channels_first") {
      weight_shape.at(0) = in->shape().At(1);
      weight_shape.at(1) = filters;
    } else if (data_format == "channels_last") {
      weight_shape.at(0) = in->shape().At(NDims + 1);
      weight_shape.at(NDims + 1) = filters;
    } else {
      UNIMPLEMENTED_THEN_RETURN();
    }
    for (size_t i = 0; i < NDims; ++i) { weight_shape.at(idx_offset + i) = kernel_size.at(i); }

    const user_op::TensorDesc* weight = ctx->TensorDesc4ArgNameAndIndex("weight", 0);
    CHECK_EQ(weight->shape(), Shape(weight_shape));
  }

  return Maybe<void>::Ok();
}

Maybe<void> InferBatchAxis4DeConv(user_op::BatchAxisContext* ctx) {
  *ctx->BatchAxis4ArgNameAndIndex("out", 0) = *ctx->BatchAxis4ArgNameAndIndex("in", 0);
  return Maybe<void>::Ok();
}

Maybe<void> GetSbpSignatures4DeConv(user_op::SbpContext* ctx) {
  ctx->NewBuilder()
      .Split(user_op::OpArg("in", 0), 0)
      .Broadcast(user_op::OpArg("weight", 0))
      .Split(user_op::OpArg("out", 0), 0)
      .Build();

  return Maybe<void>::Ok();
}

template<size_t NDims>
Maybe<void> CheckAttr(const user_op::UserOpDefWrapper& def,
                      const user_op::UserOpConfWrapper& conf) {
  bool is_checked = true;
  std::stringstream err;
  err << "Illegal value for " << conf.op_type_name() << " op " << conf.op_name() << ": ";

  const std::string& data_format = conf.attr<std::string>("data_format");
  if (!(data_format == "channels_first" || data_format == "channels_last")) {
    err << " data_format:" << data_format;
    is_checked = false;
  }

  const std::string& padding = conf.attr<std::string>("padding");
  if (padding != "valid") {  // only support vaild for now
    err << " padding:" << padding;
    is_checked = false;
  }

  if (NDims != 0) {
    const auto& kernel_size = conf.attr<std::vector<int32_t>>("kernel_size");
    if (kernel_size.size() != NDims) {
      err << " kernel_size: number of element is " << kernel_size.size();
      is_checked = false;
    }

    const auto& output_padding = conf.attr<std::vector<int32_t>>("output_padding");
    if (output_padding.size() != NDims) {
      err << " output_padding: number of element is " << output_padding.size();
      is_checked = false;
    }

    const auto& strides = conf.attr<std::vector<int32_t>>("strides");
    if (strides.size() != NDims) {
      err << " strides: number of element is " << strides.size();
      is_checked = false;
    }

    const auto& dilation_rate = conf.attr<std::vector<int32_t>>("dilation_rate");
    if (dilation_rate.size() != NDims) {
      err << " dilation_rate: number of element is " << dilation_rate.size();
      is_checked = false;
    }
  }

  if (is_checked) {
    return Maybe<void>::Ok();
  } else {
    return oneflow::Error::CheckFailed() << err.str();
  }
}

void GenerateBackwardOpConf4DeConv(const user_op::UserOpWrapper& op, user_op::AddOpFn AddOp) {
  const std::string& padding = op.attr<std::string>("padding");
  const std::string& data_format = op.attr<std::string>("data_format");
  const auto& kernel_size = op.attr<std::vector<int32_t>>("kernel_size");
  const auto& strides = op.attr<std::vector<int32_t>>("strides");
  const auto& dilation_rate = op.attr<std::vector<int32_t>>("dilation_rate");
  const Shape& weight_shape = op.TensorDesc4ArgNameAndIndex("weight", 0).shape();

  const int32_t ndims = kernel_size.size();
  CHECK_EQ(ndims, strides.size());
  CHECK_EQ(ndims, dilation_rate.size());

  if (op.NeedGenGradTensor4OpInput("weight", 0)) {
    auto filter_grad_op =
        user_op::UserOpConfWrapperBuilder("System-AutoGrad-" + op.op_name() + "-FilterGrad")
            .Op("conv_filter_grad")
            .Input("dy", op.input("in", 0))
            .Input("x", op.GetGradTensorWithOpOutput("out", 0))
            .Output("filter_diff")
            .Attr<int32_t>("num_spatial_dims", ndims)
            .Attr<std::string>("padding", padding)
            .Attr<std::string>("data_format", data_format)
            .Attr<std::vector<int32_t>>("kernel_size", kernel_size)
            .Attr<std::vector<int32_t>>("strides", strides)
            .Attr<std::vector<int32_t>>("dilation_rate", dilation_rate)
            .Attr<int32_t>("groups", 1)
            .Build();
    op.BindGradTensorWithOpInput(filter_grad_op.output("filter_diff", 0), "weight", 0);
    AddOp(filter_grad_op);
  }

  if (op.NeedGenGradTensor4OpInput("in", 0)) {
    std::string ndims_str = std::to_string(ndims);
    auto data_grad_op =
        user_op::UserOpConfWrapperBuilder("System-AutoGrad-" + op.op_name() + "-DataGrad")
            .Op("conv" + ndims_str + "d")
            .Input("in", op.GetGradTensorWithOpOutput("out", 0))
            .Input("weight", op.input("weight", 0))
            .Output("out")
            .Attr<int32_t>("filters", weight_shape.At(0))
            .Attr<std::string>("data_format", data_format)
            .Attr<std::string>("padding", padding)
            .Attr<std::vector<int32_t>>("kernel_size", kernel_size)
            .Attr<std::vector<int32_t>>("strides", strides)
            .Attr<std::vector<int32_t>>("dilation_rate", dilation_rate)
            .Attr<int32_t>("groups", 1)
            .Build();
    op.BindGradTensorWithOpInput(data_grad_op.output("out", 0), "in", 0);
    AddOp(data_grad_op);
  }
}

}  // namespace

REGISTER_USER_OP("deconv1d")
    .Input("in")
    .Input("weight")
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("output_padding", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<1>)
    .SetTensorDescInferFn(InferTensorDesc4DeConv<1>)
    .SetBatchAxisInferFn(InferBatchAxis4DeConv)
    .SetGetSbpFn(GetSbpSignatures4DeConv);

REGISTER_USER_OP("deconv2d")
    .Input("in")
    .Input("weight")
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("output_padding", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<2>)
    .SetTensorDescInferFn(InferTensorDesc4DeConv<2>)
    .SetBatchAxisInferFn(InferBatchAxis4DeConv)
    .SetGetSbpFn(GetSbpSignatures4DeConv);

REGISTER_USER_OP("deconv3d")
    .Input("in")
    .Input("weight")
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("output_padding", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<3>)
    .SetTensorDescInferFn(InferTensorDesc4DeConv<3>)
    .SetBatchAxisInferFn(InferBatchAxis4DeConv)
    .SetGetSbpFn(GetSbpSignatures4DeConv);

REGISTER_USER_OP_GRAD("deconv1d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4DeConv);
REGISTER_USER_OP_GRAD("deconv2d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4DeConv);
REGISTER_USER_OP_GRAD("deconv3d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4DeConv);

}  // namespace oneflow
