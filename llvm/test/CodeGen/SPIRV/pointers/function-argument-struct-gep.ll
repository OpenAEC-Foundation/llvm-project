; RUN: llc -verify-machineinstrs -O0 -mtriple=spirv64-unknown-unknown %s -o - | FileCheck %s
; RUN: %if spirv-tools %{ llc -O0 -mtriple=spirv64-unknown-unknown %s -o - -filetype=obj | spirv-val %}

%struct.Pair = type { float, float }

; CHECK-DAG: %[[#FLOAT:]] = OpTypeFloat 32
; CHECK-DAG: %[[#PAIR:]] = OpTypeStruct %[[#FLOAT]] %[[#FLOAT]]
; CHECK-DAG: %[[#PAIRPTR:]] = OpTypePointer Function %[[#PAIR]]
; CHECK-DAG: %[[#SUMTYPE:]] = OpTypeFunction %[[#FLOAT]] %[[#PAIRPTR]]
; CHECK-NOT: OpBitcast
; CHECK: OpFunctionCall %[[#FLOAT]] %[[#SUM:]]
; CHECK: %[[#SUM]] = OpFunction %[[#FLOAT]] None %[[#SUMTYPE]]
; CHECK-NEXT: %[[#PARAMETER:]] = OpFunctionParameter %[[#PAIRPTR]]

define void @call_sum(ptr addrspace(1) %output) {
entry:
  %pair = alloca %struct.Pair
  %result = call float @sum(ptr %pair)
  store float %result, ptr addrspace(1) %output
  ret void
}

define internal float @sum(ptr %pair) {
entry:
  %first_pointer = getelementptr inbounds %struct.Pair, ptr %pair, i32 0, i32 0
  %first = load float, ptr %first_pointer
  %second_pointer = getelementptr inbounds %struct.Pair, ptr %pair, i32 0, i32 1
  %second = load float, ptr %second_pointer
  %result = fadd float %first, %second
  ret float %result
}
