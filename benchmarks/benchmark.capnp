@0xbf5147cbbecf40c1;

# Benchmark data structures for ZMEM vs Cap'n Proto comparison

struct FixedObject {
   intArray @0 :List(Int32);
   floatArray @1 :List(Float32);
   doubleArray @2 :List(Float64);
}

struct NestedObject {
   v3s @0 :List(Vec3);
   id @1 :Text;
}

struct Vec3 {
   x @0 :Float64;
   y @1 :Float64;
   z @2 :Float64;
}

struct AnotherObject {
   string @0 :Text;
   anotherString @1 :Text;
   escapedText @2 :Text;
   boolean @3 :Bool;
   nestedObject @4 :NestedObject;
}

struct FixedNameObject {
   name0 @0 :Text;
   name1 @1 :Text;
   name2 @2 :Text;
   name3 @3 :Text;
   name4 @4 :Text;
}

struct TestObject {
   fixedObject @0 :FixedObject;
   fixedNameObject @1 :FixedNameObject;
   anotherObject @2 :AnotherObject;
   stringArray @3 :List(Text);
   string @4 :Text;
   number @5 :Float64;
   boolean @6 :Bool;
   anotherBool @7 :Bool;
}
