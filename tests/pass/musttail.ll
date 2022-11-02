; Nothing may sit between a musttail call and its ret, so the exit has to go
; in front of the call. opt's verifier would reject the module otherwise.

declare i32 @target(i32)

define i32 @forwarder(i32 %x) {
; CHECK-LABEL: define i32 @forwarder(
; CHECK: call void @__st_enter(i64 [[ID:-?[0-9]+]])
; CHECK-NEXT: call void @__st_exit(i64 [[ID]])
; CHECK-NEXT: %r = musttail call i32 @target(i32 %x)
; CHECK-NEXT: ret i32 %r
  %r = musttail call i32 @target(i32 %x)
  ret i32 %r
}
