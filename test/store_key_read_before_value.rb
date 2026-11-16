# CRuby reads a store's receiver and key (and a Hash literal pair's key) before
# the value runs, so a value that assigns the key's variable does not change
# the key the set uses.
def two(a, b) = [a, b]
x = 1; p two(x, (x = 5))
x = 1; a = [0, 0, 0]; a[x] = (x = 2; 9); p a
x = 1; h = {}; h.store(x, (x = 5)); p h
x = 1; h = {}; h[x] = (x = 5); p h
s = "k"; h = {}; h[s] = (s = "z"; 1); p h
x = 1; h = {"a" => 1}; h.fetch(x.to_s, (x = 5)); p x
x = 1; arr = []; arr.insert(0, x, (x = 7)); p arr
x = 1; h = Hash.new; h.store(x, x += 1); p h
x = 1; h = {}; h.update(x => (x = 5)); p h
@k = :a; h = {}; h[@k] = (@k = :b; 1); p h
$g = "g"; h = {}; h[$g] = ($g = "z"; 2); p h
s = +"k"; h = {}; h[s] = (s << "z"; 1); p h
y = 1; set = -> { y = 9 }; h = {}; h[y] = (set.(); 3); p h
def m; i = 0; a = [0, 0]; a[i] = (i = 1; 7); a; end; p m
k = 1; h = {1 => "a"}; 3.times { |j| h[k] = (k += 1; j) }; p h
x = 1; ph = {1 => :a, "s" => 2}; ph[x] = (x = "s"; 9); p ph
h = {}; h2 = {}; hh = h; hh[(hh = h2; :q)] = 1; p h, h2
x = 1; h = {}; h[x] = x += 1; p h
a = [1, 2, 3]; i = 0; a[i] = a[i += 1]; p a
x = 1; h = {}; h.update(x => (x = 5)); p h
x = 1; h = {}; h.merge!(x => (x = 5)); p h
x = 1; h = {}; r = h.update(x => (x = 5)); p h
x = 1; z = {x => (x = 5)}; p z
s = "a"; z = {s => (s = "b"; 1), s => 2}; p z
k = :a; z = {k => (k = :b), k => 3}; p z
x = 1; z = {x => (x = "s"), "t" => x}; p z
x = 1; z = {x => [x = 2, x]}; p z
s = +"k"; z = {s => (s << "z"; s = +"q"; 1)}; p z
s = +"k"; h = {}; h.update(s => (s << "z"; s = +"q"; 1)); p h
s = "a"; z = {s => (s = "b"; 1), s => 2}; p z
s = +"m"; z = {s => (s = +"n"; 1)}; s << "x"; p z
