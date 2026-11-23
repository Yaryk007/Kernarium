// SPDX-License-Identifier: Apache-2.0
//
// The JNI types and function table slots the old-application layer uses.
//
// The layer includes no Android header, so the same sources build with Soong
// and, for the guest experiment, with a host compiler for armv7. The table
// layout is the JNI specification's; tests/unit/andarium/oldapp_jni_tests.cpp
// compares every index here with the JDK's jni.h (offsetof) so a mistake cannot
// reach an image.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oldapp::jni {

using jint = int32_t;
using jlong = int64_t;
using jboolean = uint8_t;
using jbyte = int8_t;
using jchar = uint16_t;
using jshort = int16_t;
using jfloat = float;
using jdouble = double;
using jsize = jint;
using jobject = void*;
using jclass = jobject;
using jstring = jobject;
using jarray = jobject;
using jbyteArray = jobject;
using jmethodID = void*;
using jfieldID = void*;

union jvalue {
    jboolean z;
    jbyte b;
    jchar c;
    jshort s;
    jint i;
    jlong j;
    jfloat f;
    jdouble d;
    jobject l;
};
static_assert(sizeof(jvalue) == 8, "jvalue is eight bytes in the JNI ABI");

// _JNIEnv and _JavaVM: the only data member is the pointer to the table.
struct Env {
    void* const* functions;
};
struct Vm {
    void* const* functions;
};

constexpr int kSlotCount = 233;  // through GetObjectRefType: where ART's table ends
constexpr int kVmSlotCount = 8;
constexpr jint JNI_OK = 0;
constexpr jint JNI_ERR = -1;
constexpr jint kRefTypeLocal = 1;
constexpr jint kReleaseCommit = 1;  // JNI_COMMIT
constexpr jint kReleaseAbort = 2;   // JNI_ABORT

// The ten result types of Call*Method, in the table's order.
enum Type : int {
    kObject, kBoolean, kByte, kChar, kShort, kInt, kLong, kFloat, kDouble, kVoid,
};
// ...Method (variable arguments), ...MethodV (va_list), ...MethodA (jvalue array).
enum Variant : int { kVarargs, kVa, kArray };

constexpr int call_slot(Type t, Variant v) { return 34 + 3 * t + v; }
constexpr int nonvirtual_slot(Type t, Variant v) { return 64 + 3 * t + v; }
constexpr int static_call_slot(Type t, Variant v) { return 114 + 3 * t + v; }
// Get/Set of fields: Object..Double (nine types, no void).
constexpr int get_field_slot(Type t) { return 95 + t; }
constexpr int set_field_slot(Type t) { return 104 + t; }
constexpr int get_static_field_slot(Type t) { return 145 + t; }
constexpr int set_static_field_slot(Type t) { return 154 + t; }
// Primitive array families: Boolean..Double (eight types), t is the Type value.
constexpr int new_array_slot(Type t) { return 174 + t; }
constexpr int get_array_elements_slot(Type t) { return 182 + t; }
constexpr int release_array_elements_slot(Type t) { return 190 + t; }
constexpr int get_array_region_slot(Type t) { return 198 + t; }
constexpr int set_array_region_slot(Type t) { return 206 + t; }

namespace slot {
enum : int {
    GetVersion = 4,
    FindClass = 6,
    ToReflectedMethod = 9,
    GetSuperclass = 10,
    Throw = 13,
    ThrowNew = 14,
    ExceptionOccurred = 15,
    ExceptionClear = 17,
    PushLocalFrame = 19,
    PopLocalFrame = 20,
    NewGlobalRef = 21,
    DeleteGlobalRef = 22,
    DeleteLocalRef = 23,
    IsSameObject = 24,
    NewLocalRef = 25,
    EnsureLocalCapacity = 26,
    NewObject = 28,
    NewObjectV = 29,
    NewObjectA = 30,
    GetObjectClass = 31,
    IsInstanceOf = 32,
    GetMethodID = 33,
    GetFieldID = 94,
    GetStaticMethodID = 113,
    GetStaticFieldID = 144,
    NewString = 163,
    NewStringUTF = 167,
    GetStringUTFChars = 169,
    ReleaseStringUTFChars = 170,
    GetArrayLength = 171,
    NewObjectArray = 172,
    GetObjectArrayElement = 173,
    SetObjectArrayElement = 174,
    NewByteArray = 176,
    GetByteArrayElements = 184,
    ReleaseByteArrayElements = 192,
    GetByteArrayRegion = 200,
    SetByteArrayRegion = 208,
    GetPrimitiveArrayCritical = 222,
    ReleasePrimitiveArrayCritical = 223,
    GetStringCritical = 224,
    ReleaseStringCritical = 225,
    RegisterNatives = 215,
    MonitorEnter = 217,
    MonitorExit = 218,
    NewWeakGlobalRef = 226,
    DeleteWeakGlobalRef = 227,
    ExceptionCheck = 228,
    GetObjectRefType = 232,
};
}  // namespace slot

// JNINativeMethod: what RegisterNatives takes an array of.
struct NativeMethod {
    const char* name;
    const char* signature;
    void* function;
};

// The JavaVM invoke interface.
namespace vmslot {
enum : int {
    DestroyJavaVM = 3,
    AttachCurrentThread = 4,
    DetachCurrentThread = 5,
    GetEnv = 6,
    AttachCurrentThreadAsDaemon = 7,
};
}  // namespace vmslot

}  // namespace oldapp::jni
