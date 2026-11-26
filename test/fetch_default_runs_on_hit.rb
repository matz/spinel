# fetch's default is an argument: CRuby evaluates it on a hit too, after
# the receiver and the key.
$t = []
h = { a: 1 }
p h.fetch(:a, ($t << 1; 9)), $t
$t = []
p [10, 20].fetch(0, ($t << 2; 5)), $t
$t = []
p h.fetch(:a, ($t << 3; 1)) + ($t << 4; 1), $t
$t = []
p h.fetch(:z, ($t << 5; 7)), $t
$t = []
p h.fetch(($t << 6; :a), ($t << 7; 0)), $t
def key_of(x) = ($t << x; :a)
$t = []
p h.fetch(key_of(8), key_of(9)), $t
