; ARGS: -spantrace-min-size=4
; Functions under N IR instructions are skipped (getters, tiny wrappers).

define i32 @tiny(i32 %x) {
; CHECK-LABEL: define i32 @tiny(
; CHECK-NOT: @__st_enter
; CHECK: ret i32
  ret i32 %x
}

define i32 @big_enough(i32 %x) {
; CHECK-LABEL: define i32 @big_enough(
; CHECK: call void @__st_enter(
  %a = add i32 %x, 1
  %b = mul i32 %a, 3
  %c = sub i32 %b, %x
  ret i32 %c
}
