// A flat struct may hold a slice field (const u8[:], i32[:]): its
// { data, len } lands in the struct body, a literal builds it from a
// string literal or a view, print and .len read through the field path,
// a copy moves the whole value, and a plain assignment re-points it.
struct Named { name: const u8[:], id: i64 }
struct Ints { xs: const u8[:] }

fn show(n: Named) -> i64 { return n.id + n.name.len; }

fn main() {
    let d = Named { "abc", 42 };
    print(d.name, " ", d.id, " ", d.name.len);
    print(show(d));
    let i = Ints { d.name };
    print(" ", i.xs.len);
    var m = d;
    print(" ", m.name);
    let tail: const u8[:] = "xy";
    var v = Named { tail, 1 };
    v.name = "zz";
    print(" ", v.name, " ", v.id);
}
