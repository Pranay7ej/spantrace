; Enter goes at the top, an exit before every ret, and the module gets a name
; table plus a constructor that registers it.

; CHECK: @.st.name = private unnamed_addr constant [5 x i8] c"pick\00"
; CHECK: @.st.names = private constant [1 x { i64, ptr }] [{ i64, ptr } { i64 [[ID:-?[0-9]+]], ptr @.st.name }]
; CHECK: @llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 101, ptr @__st_module_ctor, ptr null }]

define i32 @pick(i32 %x) {
; CHECK-LABEL: define i32 @pick(
; CHECK-NEXT: entry:
; CHECK-NEXT: call void @__st_enter(i64 [[ID]])
entry:
  %c = icmp sgt i32 %x, 0
  br i1 %c, label %pos, label %neg

; CHECK: pos:
; CHECK-NEXT: call void @__st_exit(i64 [[ID]])
; CHECK-NEXT: ret i32 1
pos:
  ret i32 1

; CHECK: neg:
; CHECK-NEXT: call void @__st_exit(i64 [[ID]])
; CHECK-NEXT: ret i32 -1
neg:
  ret i32 -1
}

; Declarations are left alone.
declare void @ext()

; CHECK-LABEL: define internal void @__st_module_ctor()
; CHECK-NEXT: entry:
; CHECK-NEXT: call void @__st_register_names(ptr @.st.names, i64 1)
; CHECK-NEXT: ret void
