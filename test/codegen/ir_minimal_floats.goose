// Float values live at their own width (f32/f64): literals, locals,
// arithmetic, and print through the runtime's float hooks. An f32 widens
// for mixing with f64, and print renders the shortest exact form.
fn main() {
    let a = 1.5;
    let b = 2.25;
    print(a, " ", b);
    print(a + b, " ", a * b, " ", a - b, " ", a / b);
    let f: f32 = 0.5;
    print(" ", f, " ", f * 3.0);
    var x = 10.0;
    x += 0.25;
    x *= 2.0;
    print(" ", x);
    print(" ", 1.0 / 4.0, " ", 2.0e2);
}
