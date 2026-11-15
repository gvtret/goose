// default<T>() for a flat struct with slice fields: the folded literal
// builds the given fields, and the rest keep their zero (a null slice
// pair, 0, +0.0). A scalar default is 0 or +0.0; a slice default is the
// null pair.
struct Named { name: const u8[:], id: i64 }
struct P2 { a: i64, b: f64 }

fn main() {
    let d = default<Named>();
    print(d.name.len, " ", d.id);
    let e = default<P2>();
    print(" ", e.a, " ", e.b);
    var f = Named { name: "x", id: default<i64>(), .. };
    print(" ", f.name.len, " ", f.id);
    var g = Named { name: default<const u8[:]>(), id: 5, .. };
    print(" ", g.name.len, " ", g.id);
}
