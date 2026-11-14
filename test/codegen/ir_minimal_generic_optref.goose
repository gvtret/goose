// A generic instantiation (Box<i32>) is a flat struct like any other, so
// an optional reference to one behaves like any optional struct reference:
// null and default are the null pointer, == null tests it, and a binding
// from a value points at its body (reads require narrowing first).
struct Box<T> { v: T }

fn read(b: Box<i32>?) -> i64 { if b { return b.v; } return -1; }

fn main() {
    let r: Box<i32>? = null;
    print(read(r));
    let d: Box<i32>? = default<Box<i32>?>();
    if r == null && d == null { print(" ", 0); }
    let x = Box<i32> { 7 };
    let o: Box<i32>? = x;
    if o { print(" ", read(o), " ", o.v); }
}
