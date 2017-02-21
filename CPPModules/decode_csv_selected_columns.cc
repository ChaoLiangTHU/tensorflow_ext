/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/parsing_ops.cc.
/*
 change tensorflow offical DecodeCSVOp to DecodeCSVSelectedColumnsOp that only parse the columns in field_indices of a csv file


 compile the file as follows:
 TF_INC=$(python -c 'import tensorflow as tf; print(tf.sysconfig.get_include())')
 g++ -std=c++11 -shared DecodeCSVSelectedColumns.cc -o DecodeCSVSelectedColumns.so -fPIC -I $TF_INC

 Usage in python:
 my_module= tf.load_op_library('/abs_path/decode_csv_first_n.so')

 cols=my_module.DecodeCSVSelectedColumns(value, record_defaults=record_defaults, field_indices=field_indices) #params are the same as tf.decode_csv()
 # if field_indices = tf.constant([0,1,2],dtype=tf.int32); and there are 3 columns in the csv, this function is equivelant to tf.decode_csv(value, record_defaults=record_defaults)

 for col in cols:  # in case shape reference registered in C++ does not work
 col.set_shape([])

 See the link below for more infomation on how to add an custom op for tensorflow
 https://www.tensorflow.org/versions/r0.10/how_tos/adding_an_op/

 colinliang
 Feb 21, 2017

 *
 */

#include <vector>
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/numbers.h"

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/op_kernel.h"

using namespace tensorflow;

class DecodeCSVSelectedColumnsOp: public OpKernel {
public:
	explicit DecodeCSVSelectedColumnsOp(OpKernelConstruction* ctx) :
			OpKernel(ctx), num_first_n_columns_to_extract_(-1) {
		string delim;

		OP_REQUIRES_OK(ctx, ctx->GetAttr("OUT_TYPE", &out_type_));
		OP_REQUIRES(ctx, out_type_.size() < std::numeric_limits<int>::max(),
				errors::InvalidArgument("Out type too large"));
		OP_REQUIRES_OK(ctx, ctx->GetAttr("field_delim", &delim));

		OP_REQUIRES(ctx, delim.size() == 1,
				errors::InvalidArgument("field_delim should be only 1 char"));

		delim_ = delim[0];
	}

	void Compute(OpKernelContext* ctx) override {
		const Tensor* records;
		OpInputList record_defaults;

		OP_REQUIRES_OK(ctx, ctx->input("records", &records));
		OP_REQUIRES_OK(ctx,
				ctx->input_list("record_defaults", &record_defaults));

		if (num_first_n_columns_to_extract_ < 0) {

			for (int i = 0; i < record_defaults.size(); ++i) {
				OP_REQUIRES(ctx, record_defaults[i].NumElements() < 2,
						errors::InvalidArgument(
								"There should only be 1 default per field but field ",
								i, " has ", record_defaults[i].NumElements()));
			}

			//set field_indices
			const Tensor* field_indices_tensor;
			OP_REQUIRES_OK(ctx,
					ctx->input("field_indices", &field_indices_tensor));
			auto field_indices = field_indices_tensor->flat<int>();
			field_indices_vec_.resize(record_defaults.size());
			int maxColIndex = -1;
			if (field_indices.size() != 0) {
				OP_REQUIRES(ctx, field_indices.size() == record_defaults.size(),
						errors::InvalidArgument(
								"field_indices must be empty or the same size as record_defaults"));
				for (int i = 0; i < field_indices_vec_.size(); ++i) {
					int idx = field_indices(i);
					OP_REQUIRES(ctx, idx >= 0,
							errors::InvalidArgument("the ", i,
									"field_indice is smaller than 0: the index is ",
									idx));
					field_indices_vec_[i] = idx;
					if (idx > maxColIndex)
						maxColIndex = idx;
				}
			} else {  // use default column index, 0,1,2,...
				for (int i = 0; i < field_indices_vec_.size(); ++i) {
					int idx = i;
					field_indices_vec_[i] = idx;
					if (idx > maxColIndex)
						maxColIndex = idx;
				}
			}
			num_first_n_columns_to_extract_ = maxColIndex + 1;
		}

		//
		auto records_t = records->flat<string>();
		int64 records_size = records_t.size();

		OpOutputList output;
		OP_REQUIRES_OK(ctx, ctx->output_list("output", &output));

		for (int i = 0; i < static_cast<int>(out_type_.size()); ++i) {
			Tensor* out = nullptr;
			output.allocate(i, records->shape(), &out);
		}

		for (int64 i = 0; i < records_size; ++i) {
			const StringPiece record(records_t(i));
			std::vector < string > fields;
			ExtractFields(ctx, record, &fields);
			OP_REQUIRES(ctx, fields.size() == num_first_n_columns_to_extract_,
					errors::InvalidArgument("Expect at least ",
							num_first_n_columns_to_extract_,
							" fields but have ", fields.size(), " in record ",
							i));

			// Check each field in the record
			for (int f = 0; f < static_cast<int>(out_type_.size()); ++f) {
				const DataType& dtype = out_type_[f];

				auto column_value_str = fields[field_indices_vec_[f]];

				switch (dtype) {
				case DT_INT32: {
					// If this field is empty, check if default is given:
					// If yes, use default value; Otherwise report error.
					if (column_value_str.empty()) {
						OP_REQUIRES(ctx, record_defaults[f].NumElements() == 1,
								errors::InvalidArgument("Field ", f,
										" is required but missing in record ",
										i, "!"));

						output[f]->flat<int32>()(i) = record_defaults[f].flat<
								int32>()(0);
					} else {
						int32 value;
						OP_REQUIRES(ctx,
								strings::safe_strto32(column_value_str, &value),
								errors::InvalidArgument("Field ", f,
										" in record ", i,
										" is not a valid int32: ",
										column_value_str));
						output[f]->flat<int32>()(i) = value;
					}
					break;
				}
				case DT_INT64: {
					// If this field is empty, check if default is given:
					// If yes, use default value; Otherwise report error.
					if (column_value_str.empty()) {
						OP_REQUIRES(ctx, record_defaults[f].NumElements() == 1,
								errors::InvalidArgument("Field ", f,
										" is required but missing in record ",
										i, "!"));

						output[f]->flat<int64>()(i) = record_defaults[f].flat<
								int64>()(0);
					} else {
						int64 value;
						OP_REQUIRES(ctx,
								strings::safe_strto64(column_value_str, &value),
								errors::InvalidArgument("Field ", f,
										" in record ", i,
										" is not a valid int64: ",
										column_value_str));
						output[f]->flat<int64>()(i) = value;
					}
					break;
				}
				case DT_FLOAT: {
					// If this field is empty, check if default is given:
					// If yes, use default value; Otherwise report error.
					if (column_value_str.empty()) {
						OP_REQUIRES(ctx, record_defaults[f].NumElements() == 1,
								errors::InvalidArgument("Field ", f,
										" is required but missing in record ",
										i, "!"));
						output[f]->flat<float>()(i) = record_defaults[f].flat<
								float>()(0);
					} else {
						float value;
						OP_REQUIRES(ctx,
								strings::safe_strtof(column_value_str.c_str(),
										&value),
								errors::InvalidArgument("Field ", f,
										" in record ", i,
										" is not a valid float: ",
										column_value_str));
						output[f]->flat<float>()(i) = value;
					}
					break;
				}
				case DT_STRING: {
					// If this field is empty, check if default is given:
					// If yes, use default value; Otherwise report error.
					if (column_value_str.empty()) {
						OP_REQUIRES(ctx, record_defaults[f].NumElements() == 1,
								errors::InvalidArgument("Field ", f,
										" is required but missing in record ",
										i, "!"));
						output[f]->flat<string>()(i) = record_defaults[f].flat<
								string>()(0);
					} else {
						output[f]->flat<string>()(i) = column_value_str;
					}
					break;
				}
				default:
					OP_REQUIRES(ctx, false,
							errors::InvalidArgument("csv: data type ", dtype,
									" not supported in field ", f));
				}
			}
		}
	}

private:
	std::vector<DataType> out_type_;
	char delim_;
	std::vector<int> field_indices_vec_;
	int num_first_n_columns_to_extract_;

	inline void ExtractFields(OpKernelContext* ctx, StringPiece input,
			std::vector<string>* result) {
		int64 current_idx = 0;
		if (!input.empty()) {
			while (static_cast<size_t>(current_idx) < input.size()
					&& result->size() < num_first_n_columns_to_extract_) {
				if (input[current_idx] == '\n' || input[current_idx] == '\r') {
					current_idx++;
					continue;
				}

				bool quoted = false;
				if (input[current_idx] == '"') {
					quoted = true;
					current_idx++;
				}

				// This is the body of the field;
				string field;
				if (!quoted) {
					while (static_cast<size_t>(current_idx) < input.size()
							&& input[current_idx] != delim_) {
						OP_REQUIRES(ctx,
								input[current_idx] != '"'
										&& input[current_idx] != '\n'
										&& input[current_idx] != '\r',
								errors::InvalidArgument(
										"Unquoted fields cannot have quotes/CRLFs inside"));
						field += input[current_idx];
						current_idx++;
					}

					// Go to next field or the end
					current_idx++;
				} else {
					// Quoted field needs to be ended with '"' and delim or end
					while ((static_cast<size_t>(current_idx) < input.size() - 1)
							&& (input[current_idx] != '"'
									|| input[current_idx + 1] != delim_)) {
						if (input[current_idx] != '"') {
							field += input[current_idx];
							current_idx++;
						} else {
							OP_REQUIRES(ctx, input[current_idx + 1] == '"',
									errors::InvalidArgument(
											"Quote inside a string has to be "
													"escaped by another quote"));
							field += '"';
							current_idx += 2;
						}
					}

					OP_REQUIRES(ctx,
							(static_cast<size_t>(current_idx) < input.size()
									&& input[current_idx] == '"'
									&& (static_cast<size_t>(current_idx)
											== input.size() - 1
											|| input[current_idx + 1] == delim_)),
							errors::InvalidArgument(
									"Quoted field has to end with quote "
											"followed by delim or end"));

					current_idx += 2;
				}

				result->push_back(field);
			}

			// Check if the last field is missing
			if (input[input.size() - 1] == delim_)
				result->push_back(string());
		}
	}
};

REGISTER_OP("DecodeCSVSelectedColumns")
.Input("records: string")
.Input("record_defaults: OUT_TYPE")
.Input("field_indices: int32")
.Output("output: OUT_TYPE")
.Attr("OUT_TYPE: list({float,int32,int64,string})")
.Attr("field_delim: string = ','")
.SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
			// Validate the record_defaults inputs.
			for (int i = 1; i < c->num_inputs(); ++i) {
				::tensorflow::shape_inference::ShapeHandle v;
				TF_RETURN_IF_ERROR(c->WithRank(c->input(i), 1, &v));
				if (c->Value(c->Dim(v, 0)) > 1) {
					return errors::InvalidArgument(
							"Shape of a default must be a length-0 or length-1 vector");
				}
			}

			// Propagate shape of the records input.
			for (int i = 0; i < c->num_outputs(); ++i) c->set_output(i, c->input(0));
			return Status::OK();
		})
.Doc(R"doc(
Convert CSV records to tensors. Each column maps to one tensor.

RFC 4180 format is expected for the CSV records.
(https://tools.ietf.org/html/rfc4180)
Note that we allow leading and trailing spaces with int or float field.

records: Each string is a record/row in the csv and all records should have
  the same format.
record_defaults: One tensor per column of the input record, with either a
  scalar default value for that column or empty if the column is required.
field_indices:  tf.int32 tensor, must be the same size as record_defaults (or an empty tensor), E.g. tf.constant([0,1,2],dtype=tf.int32)
field_delim: delimiter to separate fields in a record.
output: Each tensor will have the same shape as records.
)doc");

REGISTER_KERNEL_BUILDER(Name("DecodeCSVSelectedColumns").Device(DEVICE_CPU), DecodeCSVSelectedColumnsOp);

