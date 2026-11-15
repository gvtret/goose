// print renders an array of flat structs as `[T { ... }, T { ... }]`,
// each element through the struct renderer, and a struct value as
// `T { field, ... }` with string fields in quotes.
struct Named { name: const u8[:], id: i64 }

fn main() {
    var d = Named { "a", 1 };
    print(d);
    d.id = 7;
    print(d);
    print(" ", Named { "", 0 });
}
