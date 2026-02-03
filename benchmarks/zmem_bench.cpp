// ZMEM Performance Benchmark
// This benchmark measures ZMEM serialization performance using Glaze

#include "glaze/zmem.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// ============================================================================
// Test Data Structures
// ============================================================================

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

// ============================================================================
// Test Data Initialization
// ============================================================================

TestObj create_test_data() {
   TestObj obj;

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
// Benchmark Utilities
// ============================================================================

template <typename Func>
double benchmark(Func&& func, size_t iterations) {
   // Warmup
   for (size_t i = 0; i < iterations / 10; ++i) {
      func();
   }

   auto start = std::chrono::high_resolution_clock::now();
   for (size_t i = 0; i < iterations; ++i) {
      func();
   }
   auto end = std::chrono::high_resolution_clock::now();

   auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
   return static_cast<double>(duration.count()) / static_cast<double>(iterations);
}

// ============================================================================
// Main Benchmark
// ============================================================================

int main() {
   constexpr size_t iterations = 100000;

   TestObj test_data = create_test_data();

   // Pre-serialize for read benchmarks
   std::string buffer;
   if (auto ec = glz::write_zmem(test_data, buffer); ec) {
      std::cerr << "ZMEM write error: " << glz::format_error(ec, buffer) << "\n";
      return 1;
   }

   std::cout << "ZMEM Benchmark\n";
   std::cout << "==============\n\n";
   std::cout << "Iterations: " << iterations << "\n";
   std::cout << "Serialized size: " << buffer.size() << " bytes\n\n";

   // Write benchmark
   std::string write_buffer;

   double write_ns = benchmark([&] {
      (void)glz::write_zmem(test_data, write_buffer);
   }, iterations);

   // Write (preallocated) benchmark - computes size first, allocates once, writes without bounds checks
   std::string prealloc_buffer;

   double write_prealloc_ns = benchmark([&] {
      (void)glz::write_zmem_preallocated(test_data, prealloc_buffer);
   }, iterations);

   // Read benchmark
   TestObj result;

   double read_ns = benchmark([&] {
      (void)glz::read_zmem(result, buffer);
   }, iterations);

   // Results
   std::cout << std::fixed << std::setprecision(1);
   std::cout << "| Operation | Time (ns) | Throughput (MB/s) |\n";
   std::cout << "|-----------|-----------|-------------------|\n";
   std::cout << "| Write | " << write_ns << " | "
             << (buffer.size() / write_ns * 1000.0) << " |\n";
   std::cout << "| Write (prealloc) | " << write_prealloc_ns << " | "
             << (buffer.size() / write_prealloc_ns * 1000.0) << " |\n";
   std::cout << "| Read | " << read_ns << " | "
             << (buffer.size() / read_ns * 1000.0) << " |\n";

   return 0;
}
