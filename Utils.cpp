//
// Copyright © 2017 Arm Ltd. All rights reserved.
// See LICENSE file in the project root for full license information.
//

#define LOG_TAG "ArmnnDriver"

#include "Utils.hpp"

#include <Permute.hpp>

#include <boost/format.hpp>
#include <log/log.h>

#include <cassert>
#include <cinttypes>
#include <fstream>
#include <iomanip>

using namespace android;
using namespace android::hidl::memory::V1_0;

namespace armnn_driver
{
const armnn::PermutationVector g_DontPermute{};

namespace
{

template <typename T>
void SwizzleAndroidNn4dTensorToArmNn(const armnn::TensorShape& inTensorShape, const void* input,
                                     void* output, const armnn::PermutationVector& mappings)
{
    const auto inputData = static_cast<const T*>(input);
    const auto outputData = static_cast<T*>(output);

    armnnUtils::Permute(armnnUtils::Permuted(inTensorShape, mappings), mappings, inputData, outputData);
}

} // anonymous namespace

void SwizzleAndroidNn4dTensorToArmNn(const armnn::TensorInfo& tensor, const void* input, void* output,
                                     const armnn::PermutationVector& mappings)
{
    assert(tensor.GetNumDimensions() == 4U);

    switch(tensor.GetDataType())
    {
    case armnn::DataType::Float32:
        SwizzleAndroidNn4dTensorToArmNn<float>(tensor.GetShape(), input, output, mappings);
        break;
    case armnn::DataType::QuantisedAsymm8:
        SwizzleAndroidNn4dTensorToArmNn<uint8_t>(tensor.GetShape(), input, output, mappings);
        break;
    default:
        ALOGW("Unknown armnn::DataType for swizzling");
        assert(0);
    }
}

void* GetMemoryFromPool(DataLocation location, const std::vector<android::nn::RunTimePoolInfo>& memPools)
{
    // find the location within the pool
    assert(location.poolIndex < memPools.size());

    const android::nn::RunTimePoolInfo& memPool = memPools[location.poolIndex];

    // Type android::nn::RunTimePoolInfo has changed between Android O and Android P, where
    // "buffer" has been made private and must be accessed via the accessor method "getBuffer".
#if defined(ARMNN_ANDROID_P) // Use the new Android P implementation.
    uint8_t* memPoolBuffer = memPool.getBuffer();
#else // Fallback to the old Android O implementation.
    uint8_t* memPoolBuffer = memPool.buffer;
#endif

    uint8_t* memory = memPoolBuffer + location.offset;

    return memory;
}

armnn::TensorInfo GetTensorInfoForOperand(const Operand& operand)
{
    armnn::DataType type;

    switch (operand.type)
    {
        case OperandType::TENSOR_FLOAT32:
            type = armnn::DataType::Float32;
            break;
        case OperandType::TENSOR_QUANT8_ASYMM:
            type = armnn::DataType::QuantisedAsymm8;
            break;
        case OperandType::TENSOR_INT32:
            type = armnn::DataType::Signed32;
            break;
        default:
            throw UnsupportedOperand(operand.type);
    }

    armnn::TensorInfo ret(operand.dimensions.size(), operand.dimensions.data(), type);

    ret.SetQuantizationScale(operand.scale);
    ret.SetQuantizationOffset(operand.zeroPoint);

    return ret;
}

std::string GetOperandSummary(const Operand& operand)
{
    return android::hardware::details::arrayToString(operand.dimensions, operand.dimensions.size()) + " " +
        toString(operand.type);
}

std::string GetModelSummary(const V1_0::Model& model)
{
    std::stringstream result;

    result << model.inputIndexes.size() << " input(s), " << model.operations.size() << " operation(s), " <<
        model.outputIndexes.size() << " output(s), " << model.operands.size() << " operand(s)" << std::endl;

    result << "Inputs: ";
    for (uint32_t i = 0; i < model.inputIndexes.size(); i++)
    {
        result << GetOperandSummary(model.operands[model.inputIndexes[i]]) << ", ";
    }
    result << std::endl;

    result << "Operations: ";
    for (uint32_t i = 0; i < model.operations.size(); i++)
    {
        result << toString(model.operations[i].type).c_str() << ", ";
    }
    result << std::endl;

    result << "Outputs: ";
    for (uint32_t i = 0; i < model.outputIndexes.size(); i++)
    {
        result << GetOperandSummary(model.operands[model.outputIndexes[i]]) << ", ";
    }
    result << std::endl;

    return result.str();
}

using DumpElementFunction = void (*)(const armnn::ConstTensor& tensor,
    unsigned int elementIndex,
    std::ofstream& fileStream);

namespace
{
template <typename ElementType, typename PrintableType = ElementType>
void DumpTensorElement(const armnn::ConstTensor& tensor, unsigned int elementIndex, std::ofstream& fileStream)
{
    const ElementType* elements = reinterpret_cast<const ElementType*>(tensor.GetMemoryArea());
    fileStream << static_cast<PrintableType>(elements[elementIndex]) << ",";
}

constexpr const char* MemoryLayoutString(const armnn::ConstTensor& tensor)
{
    const char* str = "";

    switch (tensor.GetNumDimensions())
    {
        case 4:  { str = "(BHWC) "; break; }
        case 3:  { str = "(HWC) "; break; }
        case 2:  { str = "(HW) "; break; }
        default: { str = ""; break; }
    }

    return str;
}
} // namespace

void DumpTensor(const std::string& dumpDir,
    const std::string& requestName,
    const std::string& tensorName,
    const armnn::ConstTensor& tensor)
{
    // The dump directory must exist in advance.
    const std::string fileName = boost::str(boost::format("%1%/%2%_%3%.dump") % dumpDir % requestName % tensorName);

    std::ofstream fileStream;
    fileStream.open(fileName, std::ofstream::out | std::ofstream::trunc);

    if (!fileStream.good())
    {
        ALOGW("Could not open file %s for writing", fileName.c_str());
        return;
    }

    DumpElementFunction dumpElementFunction = nullptr;

    switch (tensor.GetDataType())
    {
        case armnn::DataType::Float32:
        {
            dumpElementFunction = &DumpTensorElement<float>;
            break;
        }
        case armnn::DataType::QuantisedAsymm8:
        {
            dumpElementFunction = &DumpTensorElement<uint8_t, uint32_t>;
            break;
        }
        case armnn::DataType::Signed32:
        {
            dumpElementFunction = &DumpTensorElement<int32_t>;
            break;
        }
        default:
        {
            dumpElementFunction = nullptr;
        }
    }

    if (dumpElementFunction != nullptr)
    {
        const unsigned int numDimensions = tensor.GetNumDimensions();

        const unsigned int batch = (numDimensions == 4) ? tensor.GetShape()[numDimensions - 4] : 1;

        const unsigned int height = (numDimensions >= 3)
                                    ? tensor.GetShape()[numDimensions - 3]
                                    : (numDimensions >= 2) ? tensor.GetShape()[numDimensions - 2] : 1;

        const unsigned int width = (numDimensions >= 3)
                                   ? tensor.GetShape()[numDimensions - 2]
                                   : (numDimensions >= 1) ? tensor.GetShape()[numDimensions - 1] : 0;

        const unsigned int channels = (numDimensions >= 3) ? tensor.GetShape()[numDimensions - 1] : 1;

        fileStream << "# Number of elements " << tensor.GetNumElements() << std::endl;
        fileStream << "# Dimensions " << MemoryLayoutString(tensor);
        fileStream << "[" << tensor.GetShape()[0];
        for (unsigned int d = 1; d < numDimensions; d++)
        {
            fileStream << "," << tensor.GetShape()[d];
        }
        fileStream << "]" << std::endl;

        for (unsigned int e = 0, b = 0; b < batch; ++b)
        {
            if (numDimensions >= 4)
            {
                fileStream << "# Batch " << b << std::endl;
            }
            for (unsigned int c = 0; c < channels; c++)
            {
                if (numDimensions >= 3)
                {
                    fileStream << "# Channel " << c << std::endl;
                }
                for (unsigned int h = 0; h < height; h++)
                {
                    for (unsigned int w = 0; w < width; w++, e += channels)
                    {
                        (*dumpElementFunction)(tensor, e, fileStream);
                    }
                    fileStream << std::endl;
                }
                e -= channels - 1;
                if (c < channels)
                {
                    e -= ((height * width) - 1) * channels;
                }
            }
            fileStream << std::endl;
        }
        fileStream << std::endl;
    }
    else
    {
        fileStream << "Cannot dump tensor elements: Unsupported data type "
            << static_cast<unsigned int>(tensor.GetDataType()) << std::endl;
    }

    if (!fileStream.good())
    {
        ALOGW("An error occurred when writing to file %s", fileName.c_str());
    }
}

void ExportNetworkGraphToDotFile(const armnn::IOptimizedNetwork& optimizedNetwork,
                                 const std::string& dumpDir,
                                 const V1_0::Model& model)
{
    // The dump directory must exist in advance.
    if (dumpDir.empty())
    {
        return;
    }

    // Get the memory address of the model and convert it to a hex string (of at least a '0' character).
    size_t modelAddress = uintptr_t(&model);
    std::stringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0') << std::setw(1) << modelAddress;
    std::string modelAddressHexString = ss.str();

    // Set the name of the output .dot file.
    const std::string fileName = boost::str(boost::format("%1%/networkgraph_%2%.dot")
                                            % dumpDir
                                            % modelAddressHexString);

    ALOGV("Exporting the optimized network graph to file: %s", fileName.c_str());

    // Write the network graph to a dot file.
    std::ofstream fileStream;
    fileStream.open(fileName, std::ofstream::out | std::ofstream::trunc);

    if (!fileStream.good())
    {
        ALOGW("Could not open file %s for writing", fileName.c_str());
        return;
    }

    if (optimizedNetwork.SerializeToDot(fileStream) != armnn::Status::Success)
    {
        ALOGW("An error occurred when writing to file %s", fileName.c_str());
    }
}
} // namespace armnn_driver
