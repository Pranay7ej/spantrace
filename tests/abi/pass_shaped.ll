; Hand-written IR shaped exactly like SpanTracePass output (name table, ctor,
; enter/exit calls). Lets CI check the runtime ABI even without building the
; plugin, and is what I used to sanity check the runtime end to end.
target triple = "x86_64-pc-linux-gnu"

@.st.name = private unnamed_addr constant [4 x i8] c"fib\00"
@.st.name.1 = private unnamed_addr constant [5 x i8] c"main\00"
@.st.names = private constant [2 x { i64, ptr }] [{ i64, ptr } { i64 1001, ptr @.st.name }, { i64, ptr } { i64 1002, ptr @.st.name.1 }]
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 101, ptr @__st_module_ctor, ptr null }]

define i32 @fib(i32 %n) {
entry:
  call void @__st_enter(i64 1001)
  %small = icmp slt i32 %n, 2
  br i1 %small, label %base, label %rec

base:
  call void @__st_exit(i64 1001)
  ret i32 %n

rec:
  %a = sub i32 %n, 1
  %b = sub i32 %n, 2
  %fa = call i32 @fib(i32 %a)
  %fb = call i32 @fib(i32 %b)
  %s = add i32 %fa, %fb
  call void @__st_exit(i64 1001)
  ret i32 %s
}

define i32 @main() {
entry:
  call void @__st_enter(i64 1002)
  %r = call i32 @fib(i32 10)
  %ok = icmp eq i32 %r, 55
  %code = select i1 %ok, i32 0, i32 1
  call void @__st_exit(i64 1002)
  ret i32 %code
}

define internal void @__st_module_ctor() {
entry:
  call void @__st_register_names(ptr @.st.names, i64 2)
  ret void
}

declare void @__st_enter(i64)
declare void @__st_exit(i64)
declare void @__st_register_names(ptr, i64)
