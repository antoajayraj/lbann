////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////

#ifndef LBANN_LAYER_CONCATENATION_HPP_INCLUDED
#define LBANN_LAYER_CONCATENATION_HPP_INCLUDED

#include "lbann/layers/transform/transform.hpp"
#include "lbann/utils/exception.hpp"

namespace lbann {

/** Concatenation layer.
 *  This layer concatenates input tensors along a specified axis.
 */
template <data_layout T_layout = data_layout::DATA_PARALLEL, El::Device Dev = El::Device::CPU>
class concatenation_layer : public transform_layer {
 private:

  /** Tensor dimension to concatenation. */
  int m_concatenation_axis;
  /** Concatenation points for each child layer. */
  std::vector<int> m_concatenation_points;

  /** View into region of input tensor. */
  AbsDistMat *m_input_region_v;
  /** View into region of output tensor. */
  AbsDistMat *m_output_region_v;

 public:

  concatenation_layer(lbann_comm *comm,
                      int concatenation_axis)
    : transform_layer(comm),
      m_concatenation_axis(concatenation_axis),
      m_input_region_v(nullptr),
      m_output_region_v(nullptr) {

    // Concatenation layer has no limit on parents
    m_expected_num_parent_layers = -1;

  }

  concatenation_layer(const concatenation_layer& other)
    : transform_layer(other),
      m_concatenation_axis(other.m_concatenation_axis),
      m_concatenation_points(other.m_concatenation_points),
      m_input_region_v(other.m_input_region_v),
      m_output_region_v(other.m_output_region_v) {
    // Deep copy matrices
    if (m_input_region_v != nullptr) {
      m_input_region_v = m_input_region_v->Copy();
    }
    if (m_output_region_v != nullptr) {
      m_output_region_v = m_output_region_v->Copy();
    }
  }

  concatenation_layer& operator=(const concatenation_layer& other) {
    transform_layer::operator=(other);
    m_concatenation_axis = other.m_concatenation_axis;
    m_concatenation_points = other.m_concatenation_points;

    // Deep copy matrices
    if (m_input_region_v != nullptr)  { delete m_input_region_v; }
    if (m_output_region_v != nullptr) { delete m_output_region_v; }
    m_input_region_v = other.m_input_region_v;
    m_output_region_v = other.m_output_region_v;
    if (m_input_region_v != nullptr) {
      m_input_region_v = m_input_region_v->Copy();
    }
    if (m_output_region_v != nullptr) {
      m_output_region_v = m_output_region_v->Copy();
    }

  }

  virtual ~concatenation_layer() override {
    if (m_input_region_v != nullptr)  { delete m_input_region_v; }
    if (m_output_region_v != nullptr) { delete m_output_region_v; }
  }

  concatenation_layer* copy() const override { return new concatenation_layer(*this); }
  std::string get_type() const override { return "concatenation"; }
  data_layout get_data_layout() const override { return T_layout; }
  El::Device get_device_allocation() const override { return Dev; }

  /** Returns description of ctor params */
  std::string get_description() const override {
    std::stringstream s;
    s << " concatenation; concatenation_axis: "
      << m_concatenation_axis << " parents: ";
    for (size_t h=0; h<this->m_parent_layers.size(); h++) {
      s << this->m_parent_layers[h]->get_name() << " " << this->m_parent_layers[h]->get_type() << " ";
    }
    s << " concatenation_points: ";
    for (size_t h=0; h<this->m_concatenation_points.size(); h++) {
      s << this->m_concatenation_points[h] << " ";
    }
    s << " dataLayout: " << this->get_data_layout_string(get_data_layout());
    return s.str();
  }

  void setup_pointers() override {
    transform_layer::setup_pointers();
    std::stringstream err;
    if (get_num_parents() <= 0) {
      err << __FILE__ << " " << __LINE__ << " :: concatenation_layer: "
          << "concatenation layer has no parents";
      throw lbann_exception(err.str());
    }
  }

  void setup_matrices(const El::Grid& grid) override {
    transform_layer::setup_matrices(grid);
    if (m_input_region_v != nullptr)  { delete m_input_region_v; }
    if (m_output_region_v != nullptr) { delete m_output_region_v; }
    const auto& input = get_prev_activations();
    m_input_region_v = input.Construct(input.Grid(), input.Root());
    m_output_region_v = input.Construct(input.Grid(), input.Root());
  }

  void setup_dims() override {
    transform_layer::setup_dims();

    // Get concatenation points for first parent layer
    auto output_dims = get_input_dims(0);
    if (m_concatenation_axis >= (int) output_dims.size()) {
      std::stringstream err;
      err << get_type() << " layer \"" << get_name() << "\" "
          << "cannot concatenate along "
          << "axis " << m_concatenation_axis << " since it only has "
          << output_dims.size() << " dimensions";
      LBANN_ERROR(err.str());
    }
    m_concatenation_points.clear();
    m_concatenation_points.push_back(0);
    m_concatenation_points.push_back(output_dims[m_concatenation_axis]);

    // Get concatenation points for remaining parent layers
    for (int i = 1; i < get_num_parents(); ++i) {
      const auto& input_dims = get_input_dims(i);
      if (input_dims.size() != output_dims.size()
          || !std::equal(input_dims.begin(),
                         input_dims.begin() + m_concatenation_axis,
                         output_dims.begin())
          || !std::equal(input_dims.begin() + m_concatenation_axis + 1,
                         input_dims.end(),
                         output_dims.begin() + m_concatenation_axis + 1)) {
        std::stringstream err;
        err << get_type() << " layer \"" << get_name() << "\" "
            << "expects input tensors with dimensions ";
        for (size_t j = 0; j < output_dims.size(); ++j) {
          err << (j > 0 ? " x " : "");
          if ((int) j == m_concatenation_axis) {
            err << "X";
          } else {
            err << output_dims[j];
          }
        }
        err << ", but parent layer "
            << "\"" << m_parent_layers[i]->get_name() << "\" "
            << "outputs with dimensions ";
        for (size_t j = 0; j < input_dims.size(); ++j) {
          err << (j > 0 ? " x " : "") << input_dims[j];
        }
        LBANN_ERROR(err.str());
      }
      output_dims[m_concatenation_axis] += input_dims[m_concatenation_axis];
      m_concatenation_points.push_back(output_dims[m_concatenation_axis]);
    }

    // Update output dimensions
    set_output_dims(output_dims);

  }

  protected:

  void fp_compute() override {

    // Gradient w.r.t. input
    auto& output = get_activations();

    // Get number of contiguous regions in a tensor slice of width 1
    const auto& output_dims = get_output_dims();
    const int num_regions = std::accumulate(output_dims.begin(),
                                            output_dims.begin() + m_concatenation_axis,
                                            1,
                                            std::multiplies<int>());
    const int unit_region_size = std::accumulate(output_dims.begin() + m_concatenation_axis + 1,
                                                 output_dims.end(),
                                                 1,
                                                 std::multiplies<int>());

    // Get stride between contiguous regions in output tensor slices
    const int output_slice_dim = output_dims[m_concatenation_axis];
    const int output_region_stride = output_slice_dim * unit_region_size;

    // Populate output with slices of inputs
    for (int i = 0; i < get_num_parents(); ++i) {
      const auto& input = get_prev_activations(i);

      // Get stride between contiguous regions in input tensor slices
      const int input_slice_dim = m_concatenation_points[i+1] - m_concatenation_points[i];
      const int input_region_stride = input_slice_dim * unit_region_size;

      // Get position of first contiguous region in output tensor
      const int output_region_start = m_concatenation_points[i] * unit_region_size;
      const int output_region_end = m_concatenation_points[i+1] * unit_region_size;

      // Populate concatenation of gradient w.r.t. input
      for (int region = 0; region < num_regions; ++region) {
        El::LockedView(*m_input_region_v,
                       input,
                       El::IR(region * input_region_stride,
                              (region+1) * input_region_stride),
                       El::ALL);
        El::View(*m_output_region_v,
                 output,
                 El::IR(output_region_start + region * output_region_stride,
                        output_region_end + region * output_region_stride),
                 El::ALL);
        El::Copy(*m_input_region_v, *m_output_region_v);
      }

    }

  }

  void bp_compute() override {

    // Gradient w.r.t. output
    const auto& gradient_wrt_output = get_prev_error_signals();

    // Get number of contiguous regions in a tensor slice of width 1
    const auto& output_dims = get_output_dims();
    const int num_regions = std::accumulate(output_dims.begin(),
                                            output_dims.begin() + m_concatenation_axis,
                                            1,
                                            std::multiplies<int>());
    const int unit_region_size = std::accumulate(output_dims.begin() + m_concatenation_axis + 1,
                                                 output_dims.end(),
                                                 1,
                                                 std::multiplies<int>());

    // Get stride between contiguous regions in input tensor slice
    const int output_slice_dim = output_dims[m_concatenation_axis];
    const int output_region_stride = output_slice_dim * unit_region_size;

    // Populate output tensors with concatenations of input tensor
    for (int i = 0; i < get_num_parents(); ++i) {
      auto& gradient_wrt_input = get_error_signals(i);

      // Get stride between contiguous regions in output tensor concatenations
      const int input_slice_dim = m_concatenation_points[i+1] - m_concatenation_points[i];
      const int input_region_stride = input_slice_dim * unit_region_size;

      // Get position of first contiguous region in output tensor
      const int output_region_start = m_concatenation_points[i] * unit_region_size;
      const int output_region_end = m_concatenation_points[i+1] * unit_region_size;

      // Populate current output tensor
      for (int region = 0; region < num_regions; ++region) {
        El::LockedView(*m_output_region_v,
                       gradient_wrt_output,
                       El::IR(output_region_start + region * output_region_stride,
                              output_region_end + region * output_region_stride),
                       El::ALL);
        El::View(*m_input_region_v,
                 gradient_wrt_input,
                 El::IR(region * input_region_stride,
                        (region+1) * input_region_stride),
                 El::ALL);
        El::Copy(*m_output_region_v, *m_input_region_v);
      }
    }

  }

};

} // namespace lbann

#endif // LBANN_LAYER_CONCATENATION_HPP_INCLUDED
