; ARGS: -spantrace-filter=^keep
; Only functions whose (demangled) name matches the regex get instrumented.

; CHECK: c"keep_me\00"
; CHECK-NOT: c"skip_me\00"

define void @keep_me() {
; CHECK-LABEL: define void @keep_me(
; CHECK: call void @__st_enter(
; CHECK: call void @__st_exit(
  ret void
}

define void @skip_me() {
; CHECK-LABEL: define void @skip_me(
; CHECK-NOT: @__st_
; CHECK: ret void
  ret void
}
