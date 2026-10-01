# A global String handed through four explicit `super`s, a `**h` call
# making the parameter POLY: the chain reaches a real appender. This must
# still be refused, not compiled with the append lost (#6179), when long
# read-only chains are accepted.
class P0; def m(p, k: 0) = (p << "!"; nil); end
class P1 < P0; def m(p, k: 0) = super(p, k: 1); end
class P2 < P1; def m(p, k: 0) = super(p, k: 1); end
class P3 < P2; def m(p, k: 0) = super(p, k: 1); end
class P4 < P3; def m(p, k: 0) = super(p, k: 1); end
$g = +"g"
P4.new.m($g, **{k: 2})
p $g
