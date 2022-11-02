; A static function gets a different id from an external one with the same
; name (the id mixes in the source file), while external functions hash by
; name alone so every TU agrees. (By name alone, helper would be -7184230084922550739.)
source_filename = "a.c"

define void @shared() {
; CHECK-LABEL: define void @shared(
; CHECK: call void @__st_enter(i64 579278177429913076)
  ret void
}

define internal void @helper() {
; CHECK-LABEL: define internal void @helper(
; CHECK: call void @__st_enter(i64 4574121799819464868)
  ret void
}
