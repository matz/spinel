# A store's key that runs code runs before a value that does, as CRuby
# evaluates the receiver, then the key, then the value.
$t = []
g = {}
g[($t << 1; :k)] = [($t << 2; 1)]
p g, $t
$t = []
x = (g[($t << 1; :j)] = [*($t << 2; [1]), ($t << 3; 2)])
p x, g, $t
$t = []
a = [0, 0]
a[($t << 1; 1)] = [($t << 2; 3)]
p a, $t
$t = []
g.store(($t << 1; :s), [($t << 2; 4)])
p g, $t
