; The enter call goes after the entry block's allocas so they stay grouped at
; the top of the function.

define i32 @locals() {
; CHECK-LABEL: define i32 @locals(
; CHECK-NEXT: %a = alloca i32
; CHECK-NEXT: %b = alloca i64
; CHECK-NEXT: call void @__st_enter(
; CHECK-NEXT: store i32 1, ptr %a
  %a = alloca i32
  %b = alloca i64
  store i32 1, ptr %a
  %v = load i32, ptr %a
  ret i32 %v
}
