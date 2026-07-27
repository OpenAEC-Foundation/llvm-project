; RUN: llc -verify-machineinstrs -O0 -mtriple=spirv64-unknown-unknown %s -o - | FileCheck %s
; RUN: %if spirv-tools %{ llc -O0 -mtriple=spirv64-unknown-unknown %s -o - -filetype=obj | spirv-val %}

; CHECK-DAG: %[[#TYLONG:]] = OpTypeInt 32 0
; CHECK-DAG: %[[#TYFLOAT:]] = OpTypeFloat 32
; CHECK-DAG: %[[#TYLONGPTR:]] = OpTypePointer Function %[[#TYLONG]]
; CHECK-DAG: %[[#TYSTRUCT:]] = OpTypeStruct %[[#TYLONG]]
; CHECK-DAG: %[[#TYPAIR:]] = OpTypeStruct %[[#TYFLOAT]] %[[#TYFLOAT]]
; CHECK-DAG: %[[#CONST:]] = OpConstant %[[#TYLONG]] 3
; CHECK-DAG: %[[#TYSTRUCTPTR:]] = OpTypePointer Function %[[#TYSTRUCT]]
; CHECK-DAG: %[[#TYPAIRPTR:]] = OpTypePointer Function %[[#TYPAIR]]
; CHECK: OpFunction
; CHECK: %[[#ARGPTR1:]] = OpFunctionParameter %[[#TYLONGPTR]]
; CHECK: OpStore %[[#ARGPTR1]] %[[#CONST:]]
; CHECK: OpFunction
; CHECK: %[[#SOURCE:]] = OpVariable %[[#TYPAIRPTR:]] Function
; CHECK: %[[#DESTINATION:]] = OpVariable %[[#TYPAIRPTR]] Function
; CHECK: %[[#PAIR:]] = OpLoad %[[#TYPAIR:]] %[[#SOURCE]]
; CHECK: %[[#PAIRDESTINATION:]] = OpBitcast %[[#TYPAIRPTR]] %{{[0-9]+}}
; CHECK-NEXT: OpStore %[[#PAIRDESTINATION]] %[[#PAIR]]
; CHECK: OpFunction
; CHECK: %[[#OBJ:]] = OpFunctionParameter %[[#TYSTRUCT]]
; CHECK: %[[#ARGPTR2:]] = OpFunctionParameter %[[#TYLONGPTR]]
; CHECK: %[[#PTRTOSTRUCT:]] = OpBitcast %[[#TYSTRUCTPTR]] %[[#ARGPTR2]]
; CHECK-NEXT: OpStore %[[#PTRTOSTRUCT]] %[[#OBJ]]

%struct.S = type { i32 }
%struct.Pair = type { float, float }
%struct.__wrapper_class = type { [7 x %struct.S] }

define spir_kernel void @foo(%struct.S %arg, ptr %ptr) {
entry:
  store %struct.S %arg, ptr %ptr
  ret void
}

define spir_kernel void @bar(ptr %ptr) {
entry:
  store i32 3, ptr %ptr
  ret void
}

define spir_kernel void @pair_copy(ptr addrspace(1) %output) {
entry:
  %source = alloca %struct.Pair
  %destination = alloca %struct.Pair
  %value = load %struct.Pair, ptr %source
  store %struct.Pair %value, ptr %destination
  %copy = load %struct.Pair, ptr %destination
  %first = extractvalue %struct.Pair %copy, 0
  store float %first, ptr addrspace(1) %output
  ret void
}
