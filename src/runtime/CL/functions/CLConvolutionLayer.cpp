/*
 * Copyright (c) 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/CL/functions/CLConvolutionLayer.h"

#include "arm_compute/core/PixelValue.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/CL/CLScheduler.h"

#include <cmath>
#include <tuple>

using namespace arm_compute;

CLConvolutionLayer::CLConvolutionLayer()
    : _input_im2col_kernel(), _weights_reshape_kernel(), _input_interleave_kernel(), _weights_transposed_kernel(), _mm_kernel(), _output_col2im_kernel(), _input_im2col_reshaped(),
      _input_interleaved_reshaped(), _weights_reshaped(), _weights_transposed(), _gemm_output(), _is_first_run(false), _has_bias(false), _is_fc(false)
{
}

void CLConvolutionLayer::configure(const ICLTensor *input, const ICLTensor *weights, const ICLTensor *biases, ICLTensor *output, const PadStrideInfo &conv_info)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(weights, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(output, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, weights, output);
    ARM_COMPUTE_ERROR_ON(weights->info()->dimension(2) != input->info()->dimension(2));
    ARM_COMPUTE_ERROR_ON(weights->info()->num_dimensions() > 4);

    if(biases != nullptr)
    {
        ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(biases, 1, DataType::F16, DataType::F32);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, biases);
        ARM_COMPUTE_ERROR_ON(biases->info()->dimension(0) != weights->info()->dimension(3));
        ARM_COMPUTE_ERROR_ON(biases->info()->num_dimensions() > 1);
    }

    _has_bias     = (biases != nullptr);
    _is_first_run = true;

    // Get parameters for conv_info
    unsigned int stride_x, stride_y, pad_x, pad_y = 0;
    std::tie(stride_x, stride_y) = conv_info.stride();
    std::tie(pad_x, pad_y)       = conv_info.pad();

    bool is_same_dimension = true;
    // Make sure the input and weights have same low three dimensions
    for(int i = 0; i < 3; i++)
    {
        is_same_dimension = (is_same_dimension) && (input->info()->dimension(i) == weights->info()->dimension(i));
    }

    // Run the fully connected path if is_same_dimension is true and conv_stride_x/conv_stride_y are 1, and conv_pad_x/conv_pad_y are 0 and skip col2im
    _is_fc = (is_same_dimension) && ((stride_x & stride_y) == 1) && ((pad_x | pad_y) == 0);

    // Get convolved dimensions
    unsigned int conv_w = 0;
    unsigned int conv_h = 0;

    std::tie(conv_w, conv_h) = scaled_dimensions(input->info()->dimension(0), input->info()->dimension(1), weights->info()->dimension(0),
                                                 stride_x, stride_y, pad_x, pad_y, conv_info.round());

    // Create tensor to store the reshaped weights
    const size_t      mat_weights_cols = weights->info()->dimension(3);
    const size_t      mat_weights_rows = weights->info()->dimension(0) * weights->info()->dimension(1) * weights->info()->dimension(2) + ((_has_bias) ? 1 : 0);
    const TensorShape shape_wr(mat_weights_cols, mat_weights_rows);
    _weights_reshaped.allocator()->init(TensorInfo(shape_wr, 1, weights->info()->data_type()));

    // Create tensor to store transposed weights
    TensorShape shape_wt(mat_weights_rows * 4, static_cast<size_t>(std::ceil(mat_weights_cols / 4.f)));
    TensorInfo  info_wt(shape_wt, 1, weights->info()->data_type());
    _weights_transposed.allocator()->init(info_wt);

    // Create tensor to store im2col reshaped inputs
    const size_t mat_input_cols = mat_weights_rows;
    const size_t mat_input_rows = _is_fc ? (input->info()->dimension(3)) : (conv_w * conv_h);
    TensorShape  shape_im2col   = input->info()->tensor_shape();
    shape_im2col.set(0, mat_input_cols);
    shape_im2col.set(1, mat_input_rows);
    shape_im2col.set(2, 1);
    if(_is_fc)
    {
        shape_im2col.set(3, 1);
    }
    _input_im2col_reshaped.allocator()->init(TensorInfo(shape_im2col, 1, input->info()->data_type()));

    // Create tensor to prepare input tensor for GEMM
    TensorShape shape_interleaved = shape_im2col;
    shape_interleaved.set(0, shape_interleaved.x() * 4);
    shape_interleaved.set(1, std::ceil(static_cast<float>(shape_interleaved.y()) / 4));
    _input_interleaved_reshaped.allocator()->init(TensorInfo(shape_interleaved, 1, input->info()->data_type()));

    // Create GEMM output tensor
    TensorShape shape_gemm = _input_im2col_reshaped.info()->tensor_shape();
    shape_gemm.set(0, mat_weights_cols);
    shape_gemm.set(1, mat_input_rows);
    _gemm_output.allocator()->init(TensorInfo(shape_gemm, 1, input->info()->data_type()));

    // Configure kernels
    _input_im2col_kernel.configure(input, &_input_im2col_reshaped, std::make_pair(conv_w, conv_h), conv_info, _has_bias);
    _input_interleave_kernel.configure(&_input_im2col_reshaped, &_input_interleaved_reshaped);
    _weights_reshape_kernel.configure(weights, biases, &_weights_reshaped);
    _weights_transposed_kernel.configure(&_weights_reshaped, &_weights_transposed);
    if(_is_fc)
    {
        _mm_kernel.configure(&_input_interleaved_reshaped, &_weights_transposed, output, 1.0f);
    }
    else
    {
        _mm_kernel.configure(&_input_interleaved_reshaped, &_weights_transposed, &_gemm_output, 1.0f);
        _output_col2im_kernel.configure(&_gemm_output, output, std::make_pair(conv_w, conv_h));
    }

    // Allocate intermediate tensors
    _weights_reshaped.allocator()->allocate();
    _weights_transposed.allocator()->allocate();
    _input_im2col_reshaped.allocator()->allocate();
    _input_interleaved_reshaped.allocator()->allocate();
    _gemm_output.allocator()->allocate();
}

void CLConvolutionLayer::run()
{
    // Run weights reshaping (Runs once for every configure)
    if(_is_first_run)
    {
        _is_first_run = false;
        CLScheduler::get().enqueue(_weights_reshape_kernel);
        CLScheduler::get().enqueue(_weights_transposed_kernel);
    }

    // Run input reshaping
    CLScheduler::get().enqueue(_input_im2col_kernel);
    CLScheduler::get().enqueue(_input_interleave_kernel);

    // Runs matrix multiply on reshaped matrices
    CLScheduler::get().enqueue(_mm_kernel);

    // Reshape output matrix

    if(!_is_fc)
    {
        CLScheduler::get().enqueue(_output_col2im_kernel, false);
    }
}
