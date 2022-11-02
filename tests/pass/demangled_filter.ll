; ARGS: -spantrace-filter=^media::
; The filter sees demangled C++ names, not the mangled ones.

; media::Decoder::push(int)
define void @_ZN5media7Decoder4pushEi(i32 %x) {
; CHECK-LABEL: define void @_ZN5media7Decoder4pushEi(
; CHECK: call void @__st_enter(
  ret void
}

; util::log()
define void @_ZN4util3logEv() {
; CHECK-LABEL: define void @_ZN4util3logEv(
; CHECK-NOT: @__st_enter
; CHECK: ret void
  ret void
}
