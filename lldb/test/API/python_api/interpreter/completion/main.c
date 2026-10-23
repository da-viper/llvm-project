int callee(int callee_arg) {
  return callee_arg; // break in callee
}

int main(void) {
  int caller_local = 42;
  return callee(caller_local);
}
