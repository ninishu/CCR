; NOTE: Assertions have been autogenerated by update_test_checks.py
; RUN: opt < %s -instcombine -S | FileCheck %s

define <4 x float> @test(<4 x float> %tmp26, <4 x float> %tmp53) {
        ; (X+Y)-Y != X for fp vectors.
; CHECK-LABEL: @test(
; CHECK-NEXT:    [[TMP64:%.*]] = fadd <4 x float> %tmp26, %tmp53
; CHECK-NEXT:    [[TMP75:%.*]] = fsub <4 x float> [[TMP64]], %tmp53
; CHECK-NEXT:    ret <4 x float> [[TMP75]]
;
  %tmp64 = fadd <4 x float> %tmp26, %tmp53
  %tmp75 = fsub <4 x float> %tmp64, %tmp53
  ret <4 x float> %tmp75
}