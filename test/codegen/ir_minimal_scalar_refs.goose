// A reference names another variable's storage (section 3.3): an `i64&`
// parameter reads and writes through it, a `let px .= n` binding is a
// pointer that reads as the pointee's value in every operand, a plain
// assignment to it stores into the pointee, and a rebindable `var r .= n`
// can be re-pointed with `r .= m` before writing through it again.

fn bump(x: i64&) { x += 1; }
fn get(x: i64&) -> i64 { return x; }
fn swap(a: i64&, b: i64&) { let t = a; a = b; b = t; }

fn main() {
    var n = 1;
    bump(n);
    print(n);
    let px .= n;
    px += 5;
    print(px, " ", n);
    px = 3;
    print(n);
    var r .= n;
    r = 9;
    print(r, " ", n);
    var m = 4;
    r .= m;
    r = 5;
    print(r, " ", n, " ", m);
    swap(n, m);
    print(n, " ", m);
    print(get(m));
}
