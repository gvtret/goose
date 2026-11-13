// A loop expression leaves only by its breaks, a reference may name a
// struct's field and writes it at the field's own width, and a tail-recursive
// function reads the parameters the loop it became reassigns each round.
fn countdown(n: i64) -> i64 {
    var m = n;
    loop {
        if m == 0 { break; }
        m -= 1;
    }
    return m;
}

recursive fn leading(n: u64, base: u64) -> u64 {
    let q = n / base;
    if q == 0 { return n; }
    return leading(q, base + 1);
}

struct P { a: i64, b: u8 }

fn main() {
    var p = P { a: 7, b: 250 };
    let rb = &p.b;
    rb += 10;
    let ra = &p.a;
    ra *= 2;
    print(countdown(5), " ", leading(1000000, 2), " ", p.a, " ", p.b);
}
