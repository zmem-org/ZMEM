// ZMEM vs Cap'n Proto vs FlatBuffers Performance Benchmark

#include "bencher/bencher.hpp"
#include "bencher/diagnostics.hpp"

#include "glaze/zmem.hpp"

// Cap'n Proto
#include "benchmark.capnp.h"
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

// FlatBuffers
#include "benchmark_generated.h"

#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// ZMEM Data Structures (using Glaze reflection)
// ============================================================================

namespace zmem_data {

struct Vec3 {
   double x{};
   double y{};
   double z{};
};

struct NestedObject {
   std::vector<Vec3> v3s{};
   std::string id{};
};

struct AnotherObject {
   std::string string{};
   std::string another_string{};
   std::string escaped_text{};
   bool boolean{};
   NestedObject nested_object{};
};

struct FixedObject {
   std::vector<int32_t> int_array{};
   std::vector<float> float_array{};
   std::vector<double> double_array{};
};

struct FixedNameObject {
   std::string name0{};
   std::string name1{};
   std::string name2{};
   std::string name3{};
   std::string name4{};
};

struct TestObj {
   FixedObject fixed_object{};
   FixedNameObject fixed_name_object{};
   AnotherObject another_object{};
   std::vector<std::string> string_array{};
   std::string string{};
   double number{};
   bool boolean{};
   bool another_bool{};
};

} // namespace zmem_data

// ============================================================================
// ZMEM Zero-Copy Access Helpers
// ============================================================================

// Zero-copy read using lazy_zmem API
// Accesses actual data values to ensure fair comparison with other formats
size_t read_zmem_zero_copy_lazy(const std::string& buffer) {
   glz::lazy_zmem_view<zmem_data::TestObj> view{buffer};
   size_t checksum = 0;

   // Field 0: FixedObject - sum array values
   auto fixed_obj = view.get<0>();
   auto int_array = fixed_obj.get<0>();
   auto float_array = fixed_obj.get<1>();
   auto double_array = fixed_obj.get<2>();
   for (auto v : int_array) checksum += static_cast<size_t>(v);
   for (auto v : float_array) checksum += static_cast<size_t>(v);
   for (auto v : double_array) checksum += static_cast<size_t>(v);

   // Field 1: FixedNameObject - sum string lengths
   auto name_obj = view.get<1>();
   checksum += name_obj.get<0>().size();
   checksum += name_obj.get<1>().size();
   checksum += name_obj.get<2>().size();
   checksum += name_obj.get<3>().size();
   checksum += name_obj.get<4>().size();

   // Field 2: AnotherObject - sum string lengths and bool
   auto another_obj = view.get<2>();
   checksum += another_obj.get<0>().size();
   checksum += another_obj.get<1>().size();
   checksum += another_obj.get<2>().size();
   checksum += another_obj.get<3>() ? 1 : 0;

   // Nested: NestedObject - sum Vec3 values and string length
   auto nested = another_obj.get<4>();
   auto v3s = nested.get<0>();
   for (const auto& v : v3s) {
      checksum += static_cast<size_t>(v.x + v.y + v.z);
   }
   checksum += nested.get<1>().size();

   // Field 3: string_array (variable vector - just count)
   auto str_arr = view.get<3>();
   checksum += str_arr.second; // count

   // Field 4: string length
   checksum += view.get<4>().size();

   // Fields 5-7: number, boolean, another_bool
   checksum += static_cast<size_t>(view.get<5>());
   checksum += view.get<6>() ? 1 : 0;
   checksum += view.get<7>() ? 1 : 0;

   return checksum;
}


// ============================================================================
// Test Data Initialization
// ============================================================================

zmem_data::TestObj create_test_data() {
   zmem_data::TestObj obj;

   // Fixed object
   obj.fixed_object.int_array = {0, 1, 2, 3, 4, 5, 6};
   obj.fixed_object.float_array = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
   obj.fixed_object.double_array = {3288398.238, 233e22, 289e-1, 0.928759872, 0.22222848, 0.1, 0.2, 0.3, 0.4};

   // Fixed name object
   obj.fixed_name_object.name0 = "James";
   obj.fixed_name_object.name1 = "Abraham";
   obj.fixed_name_object.name2 = "Susan";
   obj.fixed_name_object.name3 = "Frank";
   obj.fixed_name_object.name4 = "Alicia";

   // Another object
   obj.another_object.string = "here is some text";
   obj.another_object.another_string = "Hello World";
   obj.another_object.escaped_text = R"({"some key":"some string value"})";
   obj.another_object.boolean = false;
   obj.another_object.nested_object.v3s = {
      {0.12345, 0.23456, 0.001345},
      {0.3894675, 97.39827, 297.92387},
      {18.18, 87.289, 2988.298}
   };
   obj.another_object.nested_object.id = "298728949872";

   // String array
   obj.string_array = {"Cat", "Dog", "Elephant", "Tiger"};

   // Simple fields
   obj.string = "Hello world";
   obj.number = 3.14;
   obj.boolean = true;
   obj.another_bool = false;

   return obj;
}

// ============================================================================
// Cap'n Proto Helpers
// ============================================================================

void populate_capnp(::capnp::MessageBuilder& message, const zmem_data::TestObj& obj) {
   auto root = message.initRoot<::TestObject>();

   // Fixed object
   auto fixed = root.initFixedObject();
   auto intArray = fixed.initIntArray(obj.fixed_object.int_array.size());
   for (size_t i = 0; i < obj.fixed_object.int_array.size(); ++i) {
      intArray.set(i, obj.fixed_object.int_array[i]);
   }
   auto floatArray = fixed.initFloatArray(obj.fixed_object.float_array.size());
   for (size_t i = 0; i < obj.fixed_object.float_array.size(); ++i) {
      floatArray.set(i, obj.fixed_object.float_array[i]);
   }
   auto doubleArray = fixed.initDoubleArray(obj.fixed_object.double_array.size());
   for (size_t i = 0; i < obj.fixed_object.double_array.size(); ++i) {
      doubleArray.set(i, obj.fixed_object.double_array[i]);
   }

   // Fixed name object
   auto fixedName = root.initFixedNameObject();
   fixedName.setName0(obj.fixed_name_object.name0);
   fixedName.setName1(obj.fixed_name_object.name1);
   fixedName.setName2(obj.fixed_name_object.name2);
   fixedName.setName3(obj.fixed_name_object.name3);
   fixedName.setName4(obj.fixed_name_object.name4);

   // Another object
   auto another = root.initAnotherObject();
   another.setString(obj.another_object.string);
   another.setAnotherString(obj.another_object.another_string);
   another.setEscapedText(obj.another_object.escaped_text);
   another.setBoolean(obj.another_object.boolean);

   auto nested = another.initNestedObject();
   auto v3s = nested.initV3s(obj.another_object.nested_object.v3s.size());
   for (size_t i = 0; i < obj.another_object.nested_object.v3s.size(); ++i) {
      v3s[i].setX(obj.another_object.nested_object.v3s[i].x);
      v3s[i].setY(obj.another_object.nested_object.v3s[i].y);
      v3s[i].setZ(obj.another_object.nested_object.v3s[i].z);
   }
   nested.setId(obj.another_object.nested_object.id);

   // String array
   auto stringArray = root.initStringArray(obj.string_array.size());
   for (size_t i = 0; i < obj.string_array.size(); ++i) {
      stringArray.set(i, obj.string_array[i]);
   }

   // Simple fields
   root.setString(obj.string);
   root.setNumber(obj.number);
   root.setBoolean(obj.boolean);
   root.setAnotherBool(obj.another_bool);
}

// Zero-copy read: access all fields through Cap'n Proto accessors without copying
size_t read_capnp_zero_copy(::capnp::MessageReader& message) {
   auto root = message.getRoot<::TestObject>();
   size_t checksum = 0;

   // Fixed object - sum array values
   auto fixed = root.getFixedObject();
   for (auto val : fixed.getIntArray()) checksum += static_cast<size_t>(val);
   for (auto val : fixed.getFloatArray()) checksum += static_cast<size_t>(val);
   for (auto val : fixed.getDoubleArray()) checksum += static_cast<size_t>(val);

   // Fixed name object - sum string lengths
   auto fixedName = root.getFixedNameObject();
   checksum += fixedName.getName0().size();
   checksum += fixedName.getName1().size();
   checksum += fixedName.getName2().size();
   checksum += fixedName.getName3().size();
   checksum += fixedName.getName4().size();

   // Another object
   auto another = root.getAnotherObject();
   checksum += another.getString().size();
   checksum += another.getAnotherString().size();
   checksum += another.getEscapedText().size();
   checksum += another.getBoolean() ? 1 : 0;

   auto nested = another.getNestedObject();
   for (auto v3 : nested.getV3s()) {
      checksum += static_cast<size_t>(v3.getX() + v3.getY() + v3.getZ());
   }
   checksum += nested.getId().size();

   // String array - sum lengths
   for (auto s : root.getStringArray()) {
      checksum += s.size();
   }

   // Simple fields
   checksum += root.getString().size();
   checksum += static_cast<size_t>(root.getNumber());
   checksum += root.getBoolean() ? 1 : 0;
   checksum += root.getAnotherBool() ? 1 : 0;

   return checksum;
}

zmem_data::TestObj read_capnp(::capnp::MessageReader& message) {
   zmem_data::TestObj obj;
   auto root = message.getRoot<::TestObject>();

   // Fixed object
   auto fixed = root.getFixedObject();
   for (auto val : fixed.getIntArray()) {
      obj.fixed_object.int_array.push_back(val);
   }
   for (auto val : fixed.getFloatArray()) {
      obj.fixed_object.float_array.push_back(val);
   }
   for (auto val : fixed.getDoubleArray()) {
      obj.fixed_object.double_array.push_back(val);
   }

   // Fixed name object
   auto fixedName = root.getFixedNameObject();
   obj.fixed_name_object.name0 = fixedName.getName0();
   obj.fixed_name_object.name1 = fixedName.getName1();
   obj.fixed_name_object.name2 = fixedName.getName2();
   obj.fixed_name_object.name3 = fixedName.getName3();
   obj.fixed_name_object.name4 = fixedName.getName4();

   // Another object
   auto another = root.getAnotherObject();
   obj.another_object.string = another.getString();
   obj.another_object.another_string = another.getAnotherString();
   obj.another_object.escaped_text = another.getEscapedText();
   obj.another_object.boolean = another.getBoolean();

   auto nested = another.getNestedObject();
   for (auto v3 : nested.getV3s()) {
      obj.another_object.nested_object.v3s.push_back({v3.getX(), v3.getY(), v3.getZ()});
   }
   obj.another_object.nested_object.id = nested.getId();

   // String array
   for (auto s : root.getStringArray()) {
      obj.string_array.push_back(std::string(s));
   }

   // Simple fields
   obj.string = root.getString();
   obj.number = root.getNumber();
   obj.boolean = root.getBoolean();
   obj.another_bool = root.getAnotherBool();

   return obj;
}

// ============================================================================
// FlatBuffers Helpers
// ============================================================================

flatbuffers::Offset<benchmark::TestObject> build_flatbuffer(
   flatbuffers::FlatBufferBuilder& builder, const zmem_data::TestObj& obj) {

   // Build nested structures first (FlatBuffers requires bottom-up construction)

   // Fixed object
   auto int_array = builder.CreateVector(obj.fixed_object.int_array);
   auto float_array = builder.CreateVector(obj.fixed_object.float_array);
   auto double_array = builder.CreateVector(obj.fixed_object.double_array);
   auto fixed_object = benchmark::CreateFixedObject(builder, int_array, float_array, double_array);

   // Fixed name object
   auto name0 = builder.CreateString(obj.fixed_name_object.name0);
   auto name1 = builder.CreateString(obj.fixed_name_object.name1);
   auto name2 = builder.CreateString(obj.fixed_name_object.name2);
   auto name3 = builder.CreateString(obj.fixed_name_object.name3);
   auto name4 = builder.CreateString(obj.fixed_name_object.name4);
   auto fixed_name_object = benchmark::CreateFixedNameObject(builder, name0, name1, name2, name3, name4);

   // Nested object (inside another object)
   std::vector<benchmark::Vec3> v3s_vec;
   for (const auto& v : obj.another_object.nested_object.v3s) {
      v3s_vec.push_back(benchmark::Vec3(v.x, v.y, v.z));
   }
   auto v3s = builder.CreateVectorOfStructs(v3s_vec);
   auto nested_id = builder.CreateString(obj.another_object.nested_object.id);
   auto nested_object = benchmark::CreateNestedObject(builder, v3s, nested_id);

   // Another object
   auto ao_string = builder.CreateString(obj.another_object.string);
   auto ao_another_string = builder.CreateString(obj.another_object.another_string);
   auto ao_escaped_text = builder.CreateString(obj.another_object.escaped_text);
   auto another_object = benchmark::CreateAnotherObject(builder,
      ao_string, ao_another_string, ao_escaped_text, obj.another_object.boolean, nested_object);

   // String array
   std::vector<flatbuffers::Offset<flatbuffers::String>> string_offsets;
   for (const auto& s : obj.string_array) {
      string_offsets.push_back(builder.CreateString(s));
   }
   auto string_array = builder.CreateVector(string_offsets);

   // Root string
   auto root_string = builder.CreateString(obj.string);

   // Build the root object
   return benchmark::CreateTestObject(builder,
      fixed_object,
      fixed_name_object,
      another_object,
      string_array,
      root_string,
      obj.number,
      obj.boolean,
      obj.another_bool);
}

// Zero-copy read: access all fields through FlatBuffers accessors without copying
size_t read_flatbuffer_zero_copy(const benchmark::TestObject* fb) {
   size_t checksum = 0;

   // Fixed object - sum array values
   if (auto fixed = fb->fixed_object()) {
      if (auto arr = fixed->int_array()) {
         for (auto v : *arr) checksum += static_cast<size_t>(v);
      }
      if (auto arr = fixed->float_array()) {
         for (auto v : *arr) checksum += static_cast<size_t>(v);
      }
      if (auto arr = fixed->double_array()) {
         for (auto v : *arr) checksum += static_cast<size_t>(v);
      }
   }

   // Fixed name object - sum string lengths
   if (auto fn = fb->fixed_name_object()) {
      if (fn->name0()) checksum += fn->name0()->size();
      if (fn->name1()) checksum += fn->name1()->size();
      if (fn->name2()) checksum += fn->name2()->size();
      if (fn->name3()) checksum += fn->name3()->size();
      if (fn->name4()) checksum += fn->name4()->size();
   }

   // Another object
   if (auto ao = fb->another_object()) {
      if (ao->string()) checksum += ao->string()->size();
      if (ao->another_string()) checksum += ao->another_string()->size();
      if (ao->escaped_text()) checksum += ao->escaped_text()->size();
      checksum += ao->boolean() ? 1 : 0;

      if (auto nested = ao->nested_object()) {
         if (nested->id()) checksum += nested->id()->size();
         if (auto v3s = nested->v3s()) {
            for (const auto* v : *v3s) {
               checksum += static_cast<size_t>(v->x() + v->y() + v->z());
            }
         }
      }
   }

   // String array - sum lengths
   if (auto arr = fb->string_array()) {
      for (const auto* s : *arr) {
         checksum += s->size();
      }
   }

   // Simple fields
   if (fb->string()) checksum += fb->string()->size();
   checksum += static_cast<size_t>(fb->number());
   checksum += fb->boolean() ? 1 : 0;
   checksum += fb->another_bool() ? 1 : 0;

   return checksum;
}

zmem_data::TestObj read_flatbuffer(const benchmark::TestObject* fb) {
   zmem_data::TestObj obj;

   // Fixed object
   if (auto fixed = fb->fixed_object()) {
      if (auto arr = fixed->int_array()) {
         for (auto v : *arr) obj.fixed_object.int_array.push_back(v);
      }
      if (auto arr = fixed->float_array()) {
         for (auto v : *arr) obj.fixed_object.float_array.push_back(v);
      }
      if (auto arr = fixed->double_array()) {
         for (auto v : *arr) obj.fixed_object.double_array.push_back(v);
      }
   }

   // Fixed name object
   if (auto fn = fb->fixed_name_object()) {
      if (fn->name0()) obj.fixed_name_object.name0 = fn->name0()->str();
      if (fn->name1()) obj.fixed_name_object.name1 = fn->name1()->str();
      if (fn->name2()) obj.fixed_name_object.name2 = fn->name2()->str();
      if (fn->name3()) obj.fixed_name_object.name3 = fn->name3()->str();
      if (fn->name4()) obj.fixed_name_object.name4 = fn->name4()->str();
   }

   // Another object
   if (auto ao = fb->another_object()) {
      if (ao->string()) obj.another_object.string = ao->string()->str();
      if (ao->another_string()) obj.another_object.another_string = ao->another_string()->str();
      if (ao->escaped_text()) obj.another_object.escaped_text = ao->escaped_text()->str();
      obj.another_object.boolean = ao->boolean();

      if (auto nested = ao->nested_object()) {
         if (nested->id()) obj.another_object.nested_object.id = nested->id()->str();
         if (auto v3s = nested->v3s()) {
            for (const auto* v : *v3s) {
               obj.another_object.nested_object.v3s.push_back({v->x(), v->y(), v->z()});
            }
         }
      }
   }

   // String array
   if (auto arr = fb->string_array()) {
      for (const auto* s : *arr) {
         obj.string_array.push_back(s->str());
      }
   }

   // Simple fields
   if (fb->string()) obj.string = fb->string()->str();
   obj.number = fb->number();
   obj.boolean = fb->boolean();
   obj.another_bool = fb->another_bool();

   return obj;
}

// ============================================================================
// Main Benchmark
// ============================================================================

int main() {
   // Initialize test data
   zmem_data::TestObj test_data = create_test_data();

   // Pre-serialize for read benchmarks
   std::string zmem_buffer;
   {
      auto ec = glz::write_zmem(test_data, zmem_buffer);
      if (ec) {
         std::cerr << "ZMEM write error\n";
         return 1;
      }
   }

   std::vector<kj::byte> capnp_buffer;
   {
      ::capnp::MallocMessageBuilder message;
      populate_capnp(message, test_data);
      auto array = messageToFlatArray(message);
      auto bytes = array.asBytes();
      capnp_buffer.assign(bytes.begin(), bytes.end());
   }

   std::vector<uint8_t> flatbuf_buffer;
   {
      flatbuffers::FlatBufferBuilder builder(1024);
      auto root = build_flatbuffer(builder, test_data);
      builder.Finish(root);
      flatbuf_buffer.assign(builder.GetBufferPointer(),
                            builder.GetBufferPointer() + builder.GetSize());
   }

   std::cout << "ZMEM serialized size:        " << zmem_buffer.size() << " bytes\n";
   std::cout << "Cap'n Proto serialized size: " << capnp_buffer.size() << " bytes\n";
   std::cout << "FlatBuffers serialized size: " << flatbuf_buffer.size() << " bytes\n";
   std::cout << "\n";


   // ========================================================================
   // Benchmarks
   // ========================================================================

   bencher::stage stage{"ZMEM vs Cap'n Proto vs FlatBuffers"};
   stage.baseline = "FlatBuffers Write";

   // --------------------------------------------------------------------------
   // Write Benchmarks
   // --------------------------------------------------------------------------

   std::string zmem_write_buffer;
   zmem_write_buffer.reserve(zmem_buffer.size() * 2);

   stage.run("ZMEM Write", [&] {
      zmem_write_buffer.clear();
      auto ec = glz::write_zmem(test_data, zmem_write_buffer);
      bencher::do_not_optimize(zmem_write_buffer);
      return zmem_write_buffer.size();
   });

   stage.run("Cap'n Proto Write", [&] {
      ::capnp::MallocMessageBuilder message;
      populate_capnp(message, test_data);
      auto array = messageToFlatArray(message);
      bencher::do_not_optimize(array);
      return array.asBytes().size();
   });

   flatbuffers::FlatBufferBuilder fb_builder(1024);
   stage.run("FlatBuffers Write", [&] {
      fb_builder.Clear();
      auto root = build_flatbuffer(fb_builder, test_data);
      fb_builder.Finish(root);
      bencher::do_not_optimize(fb_builder.GetBufferPointer());
      return fb_builder.GetSize();
   });

   // --------------------------------------------------------------------------
   // Read Benchmarks
   // --------------------------------------------------------------------------

   zmem_data::TestObj zmem_result;
   stage.run("ZMEM Read", [&] {
      auto ec = glz::read_zmem(zmem_result, zmem_buffer);
      bencher::do_not_optimize(zmem_result);
      return zmem_buffer.size();
   });

   stage.run("Cap'n Proto Read", [&] {
      kj::ArrayPtr<const kj::byte> bytes(capnp_buffer.data(), capnp_buffer.size());
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(bytes.begin()),
         bytes.size() / sizeof(capnp::word));
      ::capnp::FlatArrayMessageReader message(words);
      auto result = read_capnp(message);
      bencher::do_not_optimize(result);
      return capnp_buffer.size();
   });

   stage.run("FlatBuffers Read", [&] {
      auto fb = benchmark::GetTestObject(flatbuf_buffer.data());
      auto result = read_flatbuffer(fb);
      bencher::do_not_optimize(result);
      return flatbuf_buffer.size();
   });

   // --------------------------------------------------------------------------
   // Output Results (Native Types)
   // --------------------------------------------------------------------------

   bencher::print_results(stage);

   bencher::save_file(bencher::to_markdown(stage), "results.md");

   chart_config chart_cfg;
   chart_cfg.margin_bottom = 140;
   chart_cfg.font_size_bar_label = 16.0;
   bencher::save_file(bencher::bar_chart(stage, chart_cfg), "results.svg");

   // ========================================================================
   // Zero-Copy Read Benchmarks
   // ========================================================================

   bencher::stage zero_copy_stage{"Zero-Copy Read Performance"};
   zero_copy_stage.baseline = "FlatBuffers";

   // ZMEM zero-copy: access fields directly from buffer using lazy_zmem API
   zero_copy_stage.run("ZMEM", [&] {
      auto checksum = read_zmem_zero_copy_lazy(zmem_buffer);
      bencher::do_not_optimize(checksum);
      return zmem_buffer.size();
   });

   zero_copy_stage.run("Cap'n Proto", [&] {
      kj::ArrayPtr<const kj::byte> bytes(capnp_buffer.data(), capnp_buffer.size());
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(bytes.begin()),
         bytes.size() / sizeof(capnp::word));
      ::capnp::FlatArrayMessageReader message(words);
      auto checksum = read_capnp_zero_copy(message);
      bencher::do_not_optimize(checksum);
      return capnp_buffer.size();
   });

   zero_copy_stage.run("FlatBuffers", [&] {
      auto fb = benchmark::GetTestObject(flatbuf_buffer.data());
      auto checksum = read_flatbuffer_zero_copy(fb);
      bencher::do_not_optimize(checksum);
      return flatbuf_buffer.size();
   });

   // --------------------------------------------------------------------------
   // Output Results (Zero-Copy)
   // --------------------------------------------------------------------------

   bencher::print_results(zero_copy_stage);

   bencher::save_file(bencher::to_markdown(zero_copy_stage), "results_zero_copy.md");

   chart_config zero_copy_cfg;
   zero_copy_cfg.margin_bottom = 100;
   zero_copy_cfg.font_size_bar_label = 20.0;
   bencher::save_file(bencher::bar_chart(zero_copy_stage, zero_copy_cfg), "results_zero_copy.svg");

   return 0;
}
