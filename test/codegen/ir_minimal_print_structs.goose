// print renders a struct value through the default hook: the type's name,
// `{ field, field }`, strings in quotes, nested structs recursively, and
// float fields through the runtime's float hooks. default<T>() folds to a
// literal whose rest keeps its zero, so `Named { "", Inner { 0 }, 0 }`.
struct Inner { x: i64 }
struct Named { name: const u8[:], inner: Inner, id: i64 }
struct P2 { a: i64, b: f64 }

fn main() {
    let d = Named { "abc", Inner { 5 }, 42 };
    print(d);
    let e = P2 { 1, 2.5 };
    print(" ", e);
    let q = default<Named>();
    print(" ", q);
}
