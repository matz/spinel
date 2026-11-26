# a[i] op= v as a value answers the value it stored, where v rebinds the
# variable the receiver or the key reads.
a = [0, 0, 0]; p(a[2] += (a = [7]; 2)); p a
a = [1, 2, 3]; a[(a = [7, 8, 9]; [1, 2].size)] += 5; p a
@a = [1, 2, 3]; @a[(@a = [7, 8, 9]; [1, 2].size)] += 5; p @a
h = { 2 => 10 }; h[(h = { 2 => 20 }; [1, 2].size)] += 5; p h
a = [1, 2, 3]; a[(a = [7, 8, 9]; [1, 2].size)] = 5; p a
a = [1, 2, 3]; swap = -> { a = [7, 8, 9]; 1 }; a[[swap.call][0]] += 7; p a
h = { k: 1 }; p(h[:k] += (h = {}; 2)); p h
@a = [1, 2, 3]; p(@a[0] += (@a = [9]; 5)); p @a
a = [1, 2, 3]; i = 2; p(a[i] += (i = 0; 10)); p a
h = { "k" => 1.5 }; x = (h["k"] *= (h = { "k" => 0.0 }; 2)); p x, h
s = ["ab", "cd"]; p(s[1] += (s = []; "!")); p s
r = [1, 2].map { |v| a = [v, v]; a[0] += (a = [0]; v) }; p r
def m(a) = (a[1] -= (a = nil; 1))
p m([5, 6])
a = [1, 2]; p(a[1] += 3); p a
