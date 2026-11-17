// An `as` cast changes a value's width: an int widens to a float, a float
// changes width, and `as!` narrows a float to an int (truncating toward zero,
// the unchecked form). The checked narrowing of an out-of-range constant is
// folded by the optimizer and stays a later cut.
fn main() {
    let z: i64 = 5;
    let zd = z as f64;
    print(zd, " ", zd + 0.5);
    let f: f32 = 2.5;
    let d = f as f64;
    print(" ", d, " ", d + 0.25);
    let w: f64 = 44.9;
    let q = w as! i8;
    print(" ", q);
}
