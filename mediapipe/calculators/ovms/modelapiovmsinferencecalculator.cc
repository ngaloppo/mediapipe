//*****************************************************************************
// Copyright 2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include <algorithm>
#include <iostream>
#include <memory>
#include <openvino/core/type/element_type.hpp>
#include <sstream>
#include <unordered_map>

#include <adapters/inference_adapter.h>  // TODO fix path  model_api/model_api/cpp/adapters/include/adapters/inference_adapter.h
#include <openvino/core/shape.hpp>
#include <openvino/openvino.hpp>

#include "ovms.h"  // NOLINT
// #include "tfs_frontend/tfs_utils.hpp"
#include "tensorflow/core/framework/tensor.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/port/canonical_errors.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/calculators/ovms/modelapiovmsinferencecalculator.pb.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/interpreter.h"
// here we need to decide if we have several calculators (1 for OVMS repository, 1-N inside mediapipe)
// for the one inside OVMS repo it makes sense to reuse code from ovms lib
namespace mediapipe {
using std::endl;

namespace {
#define ASSERT_CAPI_STATUS_NULL(C_API_CALL)                                                 \
    {                                                                                       \
        auto* err = C_API_CALL;                                                             \
        if (err != nullptr) {                                                               \
            uint32_t code = 0;                                                              \
            const char* msg = nullptr;                                                      \
            OVMS_StatusGetCode(err, &code);                                                 \
            OVMS_StatusGetDetails(err, &msg);                                               \
            LOG(INFO) << "Error encountred in OVMSCalculator:" << msg << " code: " << code; \
            OVMS_StatusDelete(err);                                                         \
            RET_CHECK(err == nullptr);                                                      \
        }                                                                                   \
    }
#define CREATE_GUARD(GUARD_NAME, CAPI_TYPE, CAPI_PTR) \
    std::unique_ptr<CAPI_TYPE, decltype(&(CAPI_TYPE##Delete))> GUARD_NAME(CAPI_PTR, &(CAPI_TYPE##Delete));

}  // namespace

const std::string SESSION_TAG{"SESSION"};
const std::string OVTENSOR_TAG{"OVTENSOR"};
const std::string OVTENSORS_TAG{"OVTENSORS"};
const std::string TFTENSOR_TAG{"TFTENSOR"};
const std::string TFTENSORS_TAG{"TFTENSORS"};
const std::string MPTENSOR_TAG{"TENSOR"};
const std::string MPTENSORS_TAG{"TENSORS"};
const std::string TFLITE_TENSOR_TAG{"TFLITE_TENSOR"};
const std::string TFLITE_TENSORS_TAG{"TFLITE_TENSORS"};

enum class Precision {
    BF16,
    FP64,
    FP32,
    FP16,
    I64,
    I32,
    I16,
    I8,
    I4,
    U64,
    U32,
    U16,
    U8,
    U4,
    U1,
    BOOL,
    CUSTOM,
    UNDEFINED,
    DYNAMIC,
    MIXED,
    Q78,
    BIN,
    PRECISION_END
};

using TFSDataType = tensorflow::DataType;

bool startsWith(const std::string& str, const std::string& prefix) {
    auto it = prefix.begin();
    bool sizeCheck = (str.size() >= prefix.size());
    if (!sizeCheck) {
        return false;
    }
    bool allOf = std::all_of(str.begin(),
        std::next(str.begin(), prefix.size()),
        [&it](const char& c) {
            return c == *(it++);
        });
    return allOf;
}

TFSDataType getPrecisionAsDataType(Precision precision) {
    static std::unordered_map<Precision, TFSDataType> precisionMap{
        {Precision::FP32, TFSDataType::DT_FLOAT},
        {Precision::FP64, TFSDataType::DT_DOUBLE},
        {Precision::FP16, TFSDataType::DT_HALF},
        {Precision::I64, TFSDataType::DT_INT64},
        {Precision::I32, TFSDataType::DT_INT32},
        {Precision::I16, TFSDataType::DT_INT16},
        {Precision::I8, TFSDataType::DT_INT8},
        {Precision::U64, TFSDataType::DT_UINT64},
        {Precision::U16, TFSDataType::DT_UINT16},
        {Precision::U8, TFSDataType::DT_UINT8},
        //    {Precision::MIXED, TFSDataType::DT_INVALID},
        //    {Precision::Q78, TFSDataType::DT_INVALID},
        //    {Precision::BIN, TFSDataType::DT_INVALID},
        {Precision::BOOL, TFSDataType::DT_BOOL}
        //    {Precision::CUSTOM, TFSDataType::DT_INVALID}
    };
    auto it = precisionMap.find(precision);
    if (it == precisionMap.end()) {
        return TFSDataType::DT_INVALID;
    }
    return it->second;
}

static ov::Tensor convertMPTensor2OVTensor(const Tensor& inputTensor) {
    // TODO FIXME support for other types/perf
    void* data = reinterpret_cast<void*>(const_cast<float*>(inputTensor.GetCpuReadView().buffer<float>()));
    auto datatype = ov::element::f32;
    ov::Shape shape;
    for (const auto& dim : inputTensor.shape().dims) {
        shape.emplace_back(dim);
    }
    ov::Tensor result(datatype, shape, data);
    // TODO do we need to memcpy or not
    return result;
}

static Tensor convertOVTensor2MPTensor(const ov::Tensor& t) {
    // TODO FIXME support for other types/perf
    std::vector<int> rawShape;
    for (size_t i = 0; i < t.get_shape().size(); i++) {
        rawShape.emplace_back(t.get_shape()[i]);
    }
    Tensor::Shape shape{rawShape};
    Tensor outputTensor(Tensor::ElementType::kFloat32, shape);
    void* data = reinterpret_cast<void*>(const_cast<float*>(outputTensor.GetCpuWriteView().buffer<float>()));
    std::memcpy(data, t.data(), t.get_byte_size());
    return outputTensor;
}

Precision ovElementTypeToOvmsPrecision(ov::element::Type_t type) {
    static std::unordered_map<ov::element::Type_t, Precision> precisionMap{
        {ov::element::Type_t::f64, Precision::FP64},
        {ov::element::Type_t::f32, Precision::FP32},
        {ov::element::Type_t::f16, Precision::FP16},
        {ov::element::Type_t::bf16, Precision::BF16},
        {ov::element::Type_t::i64, Precision::I64},
        {ov::element::Type_t::i32, Precision::I32},
        {ov::element::Type_t::i16, Precision::I16},
        {ov::element::Type_t::i8, Precision::I8},
        {ov::element::Type_t::i4, Precision::I4},
        {ov::element::Type_t::u64, Precision::U64},
        {ov::element::Type_t::u32, Precision::U32},
        {ov::element::Type_t::u16, Precision::U16},
        {ov::element::Type_t::u8, Precision::U8},
        {ov::element::Type_t::u4, Precision::U4},
        {ov::element::Type_t::u1, Precision::U1},
        {ov::element::Type_t::undefined, Precision::UNDEFINED},
        {ov::element::Type_t::dynamic, Precision::DYNAMIC},
        //    {ov::element::Type_t::???, Precision::MIXED},
        //    {ov::element::Type_t::???, Precision::Q78},
        //    {ov::element::Type_t::???, Precision::BIN},
        {ov::element::Type_t::boolean, Precision::BOOL}
        //    {ov::element::Type_t::CUSTOM, Precision::CUSTOM}
        /*
    undefined,
    dynamic,
*/
    };
    auto it = precisionMap.find(type);
    if (it == precisionMap.end()) {
        return Precision::UNDEFINED;
    }
    return it->second;
}

ov::element::Type_t ovmsPrecisionToIE2Precision(Precision precision) {
    static std::unordered_map<Precision, ov::element::Type_t> precisionMap{
        {Precision::FP64, ov::element::Type_t::f64},
        {Precision::FP32, ov::element::Type_t::f32},
        {Precision::FP16, ov::element::Type_t::f16},
        {Precision::I64, ov::element::Type_t::i64},
        {Precision::I32, ov::element::Type_t::i32},
        {Precision::I16, ov::element::Type_t::i16},
        {Precision::I8, ov::element::Type_t::i8},
        {Precision::I4, ov::element::Type_t::i4},
        {Precision::U64, ov::element::Type_t::u64},
        {Precision::U32, ov::element::Type_t::u32},
        {Precision::U16, ov::element::Type_t::u16},
        {Precision::U8, ov::element::Type_t::u8},
        {Precision::U4, ov::element::Type_t::u4},
        {Precision::U1, ov::element::Type_t::u1},
        {Precision::BOOL, ov::element::Type_t::boolean},
        {Precision::BF16, ov::element::Type_t::bf16},
        {Precision::UNDEFINED, ov::element::Type_t::undefined},
        {Precision::DYNAMIC, ov::element::Type_t::dynamic}
        //    {Precision::MIXED, ov::element::Type_t::MIXED},
        //    {Precision::Q78, ov::element::Type_t::Q78},
        //    {Precision::BIN, ov::element::Type_t::BIN},
        //    {Precision::CUSTOM, ov::element::Type_t::CUSTOM
    };
    auto it = precisionMap.find(precision);
    if (it == precisionMap.end()) {
        return ov::element::Type_t::undefined;
    }
    return it->second;
}

Precision TFSPrecisionToOvmsPrecision(const TFSDataType& datatype) {
    static std::unordered_map<TFSDataType, Precision> precisionMap{
        {TFSDataType::DT_FLOAT, Precision::FP32},
        {TFSDataType::DT_DOUBLE, Precision::FP64},
        {TFSDataType::DT_HALF, Precision::FP16},
        {TFSDataType::DT_INT64, Precision::I64},
        {TFSDataType::DT_INT32, Precision::I32},
        {TFSDataType::DT_INT16, Precision::I16},
        {TFSDataType::DT_INT8, Precision::I8},
        {TFSDataType::DT_UINT64, Precision::U64},
        {TFSDataType::DT_UINT16, Precision::U16},
        {TFSDataType::DT_UINT8, Precision::U8},
        {TFSDataType::DT_BOOL, Precision::BOOL}};
    auto it = precisionMap.find(datatype);
    if (it == precisionMap.end()) {
        return Precision::UNDEFINED;
    }
    return it->second;
}

static tensorflow::Tensor convertOVTensor2TFTensor(const ov::Tensor& t) {
    using tensorflow::Tensor;
    using tensorflow::TensorShape;
    auto datatype = getPrecisionAsDataType(ovElementTypeToOvmsPrecision(t.get_element_type()));
    TensorShape tensorShape;
    std::vector<int64_t> rawShape;
    for (size_t i = 0; i < t.get_shape().size(); i++) {
        rawShape.emplace_back(t.get_shape()[i]);
    }
    int64_t dimsCount = rawShape.size();
    tensorflow::TensorShapeUtils::MakeShape(rawShape.data(), dimsCount, &tensorShape);
    TensorShape::BuildTensorShapeBase(rawShape, static_cast<tensorflow::TensorShapeBase<TensorShape>*>(&tensorShape));
    // here we allocate default TF CPU allocator
    tensorflow::Tensor result(datatype, tensorShape);
    void* tftensordata = result.data();
    std::memcpy(tftensordata, t.data(), t.get_byte_size());
    return result;
}

static ov::Tensor convertTFTensor2OVTensor(const tensorflow::Tensor& t) {
    void* data = t.data();
    auto datatype = ovmsPrecisionToIE2Precision(TFSPrecisionToOvmsPrecision(t.dtype()));
    ov::Shape shape;
    for (const auto& dim : t.shape()) {
        shape.emplace_back(dim.size);
    }
    ov::Tensor result(datatype, shape, data);
    return result;
}
static ov::Tensor convertTFLiteTensor2OVTensor(const TfLiteTensor& t) {
    void* data = t.data.f; // TODO probably works only for floats
    auto datatype = ov::element::f32;
    ov::Shape shape;
    // TODO FIXME HACK
    // for some reason TfLite tensor does not have bs dim
    shape.emplace_back(1);
    for (int i = 0; i < t.dims->size; ++i) {
 //       RET_CHECK_GT(t.dims->data[i], 0);
 //       num_values *= raw_tensor->dims->data[i];
        shape.emplace_back(t.dims->data[i]);
    }
    ov::Tensor result(datatype, shape, data);
    return result;
}

class ModelAPISideFeedCalculator : public CalculatorBase {
    std::shared_ptr<::InferenceAdapter> session{nullptr};
    std::unordered_map<std::string, std::string> outputNameToTag;
    std::vector<std::string> input_order_list;
    std::vector<std::string> output_order_list;
    // TODO create only if required
  std::unique_ptr<tflite::Interpreter> interpreter_ = absl::make_unique<tflite::Interpreter>();
  bool initialized = false;

public:
    static absl::Status GetContract(CalculatorContract* cc) {
        LOG(INFO) << "Main GetContract start";
        RET_CHECK(!cc->Inputs().GetTags().empty());
        RET_CHECK(!cc->Outputs().GetTags().empty());
        for (const std::string& tag : cc->Inputs().GetTags()) {
            // could be replaced with absl::StartsWith when migrated to MP
            if (startsWith(tag, OVTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to OVTensor";
                cc->Inputs().Tag(tag).Set<std::vector<ov::Tensor>>();
            } else if (startsWith(tag, OVTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to OVTensor";
                cc->Inputs().Tag(tag).Set<ov::Tensor>();
            } else if (startsWith(tag, MPTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to MPTensor";
                cc->Inputs().Tag(tag).Set<std::vector<Tensor>>();
            } else if (startsWith(tag, MPTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to MPTensor";
                cc->Inputs().Tag(tag).Set<Tensor>();
            } else if (startsWith(tag, TFTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFTensor";
                cc->Inputs().Tag(tag).Set<std::vector<tensorflow::Tensor>>();
            } else if (startsWith(tag, TFTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFTensor";
                cc->Inputs().Tag(tag).Set<tensorflow::Tensor>();
            } else if (startsWith(tag, TFLITE_TENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFLITE_Tensor";
                cc->Inputs().Tag(tag).Set<std::vector<TfLiteTensor>>();
            } else if (startsWith(tag, TFLITE_TENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFLITE_Tensor";
                cc->Inputs().Tag(tag).Set<TfLiteTensor>();
            } else {
                // TODO decide which will be easier to migrating later
                // using OV tensor by default will be more performant
                // but harder to migrate
                /*
                cc->Inputs().Tag(tag).Set<tensorflow::Tensor>();
                */
                LOG(INFO) << "setting input tag:" << tag << " to OVTensor";
                cc->Inputs().Tag(tag).Set<ov::Tensor>();
            }
        }
        for (const std::string& tag : cc->Outputs().GetTags()) {
            if (startsWith(tag, OVTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to std::vector<ov::Tensor>";
                cc->Outputs().Tag(tag).Set<std::vector<ov::Tensor>>();
            } else if (startsWith(tag, OVTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to OVTensor";
                cc->Outputs().Tag(tag).Set<ov::Tensor>();
            } else if (startsWith(tag, MPTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to MPTensor";
                cc->Outputs().Tag(tag).Set<std::vector<Tensor>>();
            } else if (startsWith(tag, MPTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to MPTensor";
                cc->Outputs().Tag(tag).Set<Tensor>();
            } else if (startsWith(tag, TFTENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFTensor";
                cc->Outputs().Tag(tag).Set<std::vector<tensorflow::Tensor>>();
            } else if (startsWith(tag, TFTENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFTensor";
                cc->Outputs().Tag(tag).Set<tensorflow::Tensor>();
            } else if (startsWith(tag, TFLITE_TENSORS_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFLITE_Tensor";
                cc->Outputs().Tag(tag).Set<std::vector<TfLiteTensor>>();
            } else if (startsWith(tag, TFLITE_TENSOR_TAG)) {
                LOG(INFO) << "setting input tag:" << tag << " to TFLITE_Tensor";
                cc->Outputs().Tag(tag).Set<TfLiteTensor>();
            } else {
                // TODO decide which will be easier to migrating later
                // using OV tensor by default will be more performant
                // but harder to migrate
                /*    
                cc->Outputs().Tag(tag).Set<tensorflow::Tensor>();
                */
                LOG(INFO) << "setting output tag:" << tag << " to OVTensor";
                cc->Outputs().Tag(tag).Set<ov::Tensor>();
            }
        }
        cc->InputSidePackets().Tag(SESSION_TAG.c_str()).Set<std::shared_ptr<::InferenceAdapter>>();
        LOG(INFO) << "Main GetContract end";
        return absl::OkStatus();
    }

    absl::Status Close(CalculatorContext* cc) final {
        LOG(INFO) << "Main Close";
        return absl::OkStatus();
    }
    absl::Status Open(CalculatorContext* cc) final {
        LOG(INFO) << "Main Open start";
        session = cc->InputSidePackets()
                      .Tag(SESSION_TAG.c_str())
                      .Get<std::shared_ptr<::InferenceAdapter>>();
        for (CollectionItemId id = cc->Inputs().BeginId();
             id < cc->Inputs().EndId(); ++id) {
            if (!cc->Inputs().Get(id).Header().IsEmpty()) {
                cc->Outputs().Get(id).SetHeader(cc->Inputs().Get(id).Header());
            }
        }
        if (cc->OutputSidePackets().NumEntries() != 0) {
            for (CollectionItemId id = cc->InputSidePackets().BeginId();
                 id < cc->InputSidePackets().EndId(); ++id) {
                cc->OutputSidePackets().Get(id).Set(cc->InputSidePackets().Get(id));
            }
        }
        const auto& options = cc->Options<ModelAPIInferenceCalculatorOptions>();
        for (const auto& [key, value] : options.tag_to_output_tensor_names()) {
            outputNameToTag[value] = key;
        }

        auto& input_list = options.input_order_list();
        input_order_list.clear();
        for(int i = 0; i < input_list.size(); i++){
            input_order_list.push_back(input_list[i]);
        }
        auto& output_list = options.output_order_list();
        output_order_list.clear();
        for(int i = 0; i < output_list.size(); i++){
            output_order_list.push_back(output_list[i]);
        }

        cc->SetOffset(TimestampDiff(0));
        LOG(INFO) << "Main Open end";
        return absl::OkStatus();
    }

    absl::Status Process(CalculatorContext* cc) final {
        LOG(INFO) << "Main process start";
        if (cc->Inputs().NumEntries() == 0) {
            return tool::StatusStop();
        }
        /////////////////////
        // PREPARE INPUT MAP
        /////////////////////

        const auto& options = cc->Options<ModelAPIInferenceCalculatorOptions>();
        const auto& inputTagInputMap = options.tag_to_input_tensor_names();
        ::InferenceInput input;
        ::InferenceOutput output;
        for (const std::string& tag : cc->Inputs().GetTags()) {
            const char* realInputName{nullptr};
            auto it = inputTagInputMap.find(tag);
            if (it == inputTagInputMap.end()) {
                realInputName = tag.c_str();
            } else {
                realInputName = it->second.c_str();
            } 
#define DESERIALIZE_TENSORS(TYPE, DESERIALIZE_FUN) \
                auto& packet = cc->Inputs().Tag(tag).Get<std::vector<TYPE>>();                \
                if ( packet.size() > 1 && input_order_list.size() != packet.size()) {                 \
                    LOG(INFO) << "input_order_list not set properly in options for multiple inputs."; \
                    RET_CHECK(false);                                                                 \
                }                                                                                     \
                if (this->input_order_list.size() > 0){                                               \
                    for (int i = 0; i < this->input_order_list.size(); i++) {                         \
                        auto& tensor = packet[i];                                                     \
                        input[this->input_order_list[i]] = DESERIALIZE_FUN(tensor);                   \
                    }                                                                                 \
                } else if (packet.size() == 1) {                                                      \
                    input[realInputName] = DESERIALIZE_FUN(packet[0]);                                \
                }

            if (startsWith(tag, OVTENSORS_TAG)) {
                DESERIALIZE_TENSORS(ov::Tensor,);
            } else if (startsWith(tag, TFLITE_TENSORS_TAG)) {
                DESERIALIZE_TENSORS(TfLiteTensor, convertTFLiteTensor2OVTensor);
            } else if (startsWith(tag, MPTENSORS_TAG)) {
                DESERIALIZE_TENSORS(Tensor, convertMPTensor2OVTensor);
            } else if (startsWith(tag, OVTENSOR_TAG)) {
                auto& packet = cc->Inputs().Tag(tag).Get<ov::Tensor>();
                input[realInputName] = packet;
#if 0
                ov::Tensor input_tensor(packet);
                const float* input_tensor_access = reinterpret_cast<float*>(input_tensor.data());
                std::stringstream ss;
                ss << "ModelAPICalculator received tensor: [ ";
                for (int x = 0; x < 10; ++x) {
                    ss << input_tensor_access[x] << " ";
                }
                ss << " ] timestamp: " << cc->InputTimestamp().DebugString() << endl;
                LOG(INFO) << ss.str();
#endif
            } else if (startsWith(tag, TFTENSOR_TAG)) {
                auto& packet = cc->Inputs().Tag(tag).Get<tensorflow::Tensor>();
                input[realInputName] = convertTFTensor2OVTensor(packet);
            } else {
                /*
                auto& packet = cc->Inputs().Tag(tag).Get<tensorflow::Tensor>();
                input[realInputName] = convertTFTensor2OVTensor(packet);
                */
                auto& packet = cc->Inputs().Tag(tag).Get<ov::Tensor>();
                input[realInputName] = packet;
            }
        }
        //////////////////
        //  INFERENCE
        //////////////////
        try {
            output = session->infer(input);
        } catch (const std::exception& e) {
            LOG(INFO) << "Catched exception from session infer():" << e.what();
            RET_CHECK(false);
        } catch (...) {
            LOG(INFO) << "Catched unknown exception from session infer()";
            RET_CHECK(false);
        }
        auto outputsCount = output.size();
        RET_CHECK(outputsCount >= cc->Outputs().GetTags().size());
        LOG(INFO) << "output tags size: " << cc->Outputs().GetTags().size();
        for (const auto& tag : cc->Outputs().GetTags()) {
            LOG(INFO) << "Processing tag: " << tag;
            std::string tensorName;
            auto it = options.tag_to_output_tensor_names().find(tag);
            if (it == options.tag_to_output_tensor_names().end()) {
                tensorName = tag;
            } else {
                tensorName = it->second;
            }
            auto tensorIt = output.find(tensorName);
            if (tensorIt == output.end()) {
                LOG(INFO) << "Could not find: " << tensorName << " in inference output";
                RET_CHECK(false);
            }
            if (startsWith(tag, OVTENSORS_TAG)) {
                LOG(INFO) << "OVMS calculator will process vector<ov::Tensor>";
                auto tensors = std::make_unique<std::vector<ov::Tensor>>();
                if ( output.size() > 1 && this->output_order_list.size() != this->output_order_list.size())
                {
                    LOG(INFO) << "output_order_list not set properly in options for multiple outputs.";
                    RET_CHECK(false);
                }
                if (this->output_order_list.size() > 0) {
                    for (int i = 0; i < this->output_order_list.size(); i++) {
                        tensorName = this->output_order_list[i];
                        tensorIt = output.find(tensorName);
                        if (tensorIt == output.end()) {
                            LOG(INFO) << "Could not find: " << tensorName << " in inference output";
                            RET_CHECK(false);
                        }
                        tensors->emplace_back(tensorIt->second);
                    }
                } else {
                    for (auto& [name,tensor] : output) {
                        tensors->emplace_back(tensor);
                    }
                }
                cc->Outputs().Tag(tag).Add(
                    tensors.release(),
                    cc->InputTimestamp());
                //break; // TODO FIXME order of outputs
                // no need to break since we only have one tag
                // create concatenator calc
            } else if (startsWith(tag, MPTENSORS_TAG)) {
                LOG(INFO) << "OVMS calculator will process vector<Tensor>";
                auto tensors = std::make_unique<std::vector<Tensor>>();
                if ( output.size() > 1 && this->output_order_list.size() != this->output_order_list.size())
                {
                    LOG(INFO) << "output_order_list not set properly in options for multiple outputs.";
                    RET_CHECK(false);
                }
                if (this->output_order_list.size() > 0) {
                    for (int i = 0; i < this->output_order_list.size(); i++) {
                        tensorName = this->output_order_list[i];
                        tensorIt = output.find(tensorName);
                        if (tensorIt == output.end()) {
                            LOG(INFO) << "Could not find: " << tensorName << " in inference output";
                            RET_CHECK(false);
                        }
                        tensors->emplace_back(convertOVTensor2MPTensor(tensorIt->second));
                    }
                } else {
                    for (auto& [name,tensor] : output) {
                        tensors->emplace_back(convertOVTensor2MPTensor(tensor));
                    }
                }
                cc->Outputs().Tag(tag).Add(
                    tensors.release(),
                    cc->InputTimestamp());
                //break; // TODO FIXME order of outputs
                // no need to break since we only have one tag
                // create concatenator calc
            } else if (startsWith(tag, TFLITE_TENSORS_TAG)) {
                // TODO FIXME use output_order_list
                LOG(INFO) << "OVMS calculator will process vector<TfLiteTensor>";
                auto outputStreamTensors = std::vector<TfLiteTensor>();
                if (!this->initialized) {
                    interpreter_->AddTensors(output.size()); // HARDCODE
                    interpreter_->SetInputs({0,1}); // HARDCODE was 0 for single input
                    size_t tensorId = 0;
                    for (auto& [name,tensor] : output) {
                        std::vector<int> tfliteshape;
                        for (auto& d : tensor.get_shape()) {
                            tfliteshape.emplace_back(d);
                        }
                        interpreter_->SetTensorParametersReadWrite(/*tensor_index*/tensorId, kTfLiteFloat32, name.c_str(),
                                                       tfliteshape, TfLiteQuantization());
                        ++tensorId;
                    }
                    interpreter_->AllocateTensors();
                    this->initialized = true;
                }
                size_t tensorId = 0;
                for (auto& [name,tensor] : output) {
                    const int interpreterTensorId = interpreter_->inputs()[tensorId];
                    TfLiteTensor* tflitetensor = interpreter_->tensor(interpreterTensorId);
                    void* tensor_ptr = tflitetensor->data.f;
                    std::memcpy(tensor_ptr, tensor.data(), tensor.get_byte_size());
                    auto tflitedims = tflitetensor->dims;
                    outputStreamTensors.emplace_back(*tflitetensor);
                    ++tensorId;
                }
                //std::reverse(outputStreamTensors.begin(), outputStreamTensors.end());
                const auto raw_box_tensor = &(outputStreamTensors)[0];
                cc->Outputs().Tag(tag).AddPacket(MakePacket<std::vector<TfLiteTensor>>(std::move(outputStreamTensors)).At( cc->InputTimestamp()));
                break;
            }else if (startsWith(tag, OVTENSOR_TAG)) {
                LOG(INFO) << "OVMS calculator will process ov::Tensor";
                cc->Outputs().Tag(tag).Add(
                    new ov::Tensor(tensorIt->second),
                    cc->InputTimestamp());
            } else if (startsWith(tag, TFTENSOR_TAG)) {
                LOG(INFO) << "OVMS calculator will process tensorflow::Tensor";
                cc->Outputs().Tag(tag).Add(
                    new tensorflow::Tensor(convertOVTensor2TFTensor(tensorIt->second)),
                    cc->InputTimestamp());
            } else {
                LOG(INFO) << "OVMS calculator will process ov::Tensor";
                /*
                cc->Outputs().Tag(tag).Add(
                    new tensorflow::Tensor(convertOVTensor2TFTensor(tensorIt->second)),
                    cc->InputTimestamp());
                    */
                cc->Outputs().Tag(tag).Add(
                    new ov::Tensor(tensorIt->second),
                    cc->InputTimestamp());
            }
            LOG(INFO) << "OVMS calculator will process TfLite tensors";
        }
        LOG(INFO) << "Main process end";
        return absl::OkStatus();
    }
};

REGISTER_CALCULATOR(ModelAPISideFeedCalculator);
}  // namespace mediapipe
