// A fixed array of flat structs: its literal builds each element in
// place, a field path reaches through an element (pair[1].name), print
// renders the whole run, and a for binds each element as a struct.
struct Named { name: const u8[:], id: i64 }

fn main() {
    let pair = [Named { "a", 1 }, Named { "bb", 2 }];
    print(pair, " ", pair.len, " ", pair[0].id, " ", pair[1].name);
    for x in pair { print(x); }
    var mut = pair;
    mut[0].id = 10;
    print(" ", mut[0].id, " ", pair[0].id);
}
