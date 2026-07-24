; RUN: %if spirv-tools %{ llc -O0 -mtriple=spirv-unknown-vulkan-compute %s -o - -filetype=obj | spirv-val --target-env vulkan1.3 %}
; RUN: llc -O0 -mtriple=spirv-unknown-vulkan-compute %s -o - | FileCheck %s

; A case construct may contain nested selections before it branches to another
; case. The target case must immediately follow the source case in the
; OpSwitch target list even when the fallthrough is not an immediate successor
; of the source case's entry block.

; CHECK: OpSwitch

target triple = "spirv-unknown-vulkan-compute"

define spir_func float @nested_case_fallthrough(float %elem, float %elem2) #0 {
entry:
  %flt.i = fcmp olt float %elem, 4.000000e+02
  %flt.i44 = fcmp olt float %elem2, 3.000000e+02
  %fsub.i = fsub float %elem, %elem2
  %fadd.i = fadd float %elem, %elem2
  br label %while_body

while_body:
  %depth.079 = phi float [ 0.000000e+00, %entry ], [ %fadd.i68, %else_body22 ]
  %step.078 = phi i32 [ 0, %entry ], [ %add.i, %else_body22 ]
  br i1 %flt.i, label %if_then, label %else_body

while_exit:
  %shade.187 = phi float [ 0.000000e+00, %else_body22 ], [ 1.000000e+00, %if_exit ]
  ret float %shade.187

if_then:
  %fadd.i41 = fadd float %fadd.i, %depth.079
  br label %if_exit

else_body:
  br i1 %flt.i44, label %if_then7, label %else_body11

if_exit:
  %sample.0 = phi float [ %fadd.i41, %if_then ], [ %fadd.i52, %if_then7 ], [ 1.000000e+00, %if_then16 ], [ %fadd.i58, %if_exit14 ], [ 0.000000e+00, %else_body11 ]
  %abs.i = tail call float @llvm.fabs.f32(float %sample.0)
  %flt.i47 = fcmp olt float %abs.i, 9.000000e-03
  br i1 %flt.i47, label %while_exit, label %else_body22

if_then7:
  %fadd.i52 = fadd float %fsub.i, %depth.079
  br label %if_exit

else_body11:
  %fadd.i55 = fadd float %elem2, %depth.079
  %fadd.i58 = fadd float %elem, %fadd.i55
  %flt.i61 = fcmp olt float %fadd.i58, 0.000000e+00
  br i1 %flt.i61, label %if_exit, label %if_exit14

if_exit14:
  %fgt.i = fcmp ogt float %fadd.i58, 1.000000e+00
  br i1 %fgt.i, label %if_then16, label %if_exit

if_then16:
  br label %if_exit

else_body22:
  %fmul.i = fmul float %abs.i, 6.400000e-01
  %fmax.i = tail call float @llvm.maxnum.f32(float %fmul.i, float 1.400000e-02)
  %fadd.i68 = fadd float %depth.079, %fmax.i
  %fgt.i71 = fcmp ogt float %fadd.i68, 5.000000e+00
  %add.i = add nuw nsw i32 %step.078, 1
  %lt.i = icmp samesign ugt i32 %step.078, 42
  %or.cond = select i1 %fgt.i71, i1 true, i1 %lt.i
  br i1 %or.cond, label %while_exit, label %while_body
}

; Region-exit routing can create switch cases that converge before the switch
; merge. The internal convergence must become the corresponding switch merge.
define spir_func float @internal_case_convergence() {
entry:
  br i1 false, label %early_return, label %outer_path

early_return:
  ret float 0.000000e+00

outer_path:
  br i1 false, label %second_return, label %inner_path

second_return:
  ret float 0.000000e+00

inner_path:
  br i1 false, label %convergence, label %nested_path

nested_path:
  br label %convergence

convergence:
  %result = phi float [ 0.000000e+00, %nested_path ],
                      [ 0.000000e+00, %inner_path ]
  ret float %result
}

declare float @llvm.fabs.f32(float)
declare float @llvm.maxnum.f32(float, float)

attributes #0 = { nofree norecurse nosync nounwind memory(readwrite, argmem: none, target_mem: none) }
