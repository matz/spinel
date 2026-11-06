# A Struct's or Data's own class method defined under a condition with a
# rest, keywords or a block: until the def runs, the call reaches the
# builtin, which takes the arguments the call passes or, for one that
# takes none, raises ArgumentError for them.
K = Struct.new(:a, keyword_init: true)
def K.new(**k) = [:mine, k] if ARGV.size < 5
p K.new(a: 1)
R = Struct.new(:a, :b)
def R.new(*r) = [:mine, r] if ARGV.size < 5
p R.new(1, 2)
L = Struct.new(:a, :b)
def L.new(*r) = [:mine, r] if ARGV.size > 5
p L.new(1, 2)

S = Struct.new(:a)
def S.new(a, &b) = b.call(a) if ARGV.size < 5
p S.new(3) { |x| x * 2 }
T = Struct.new(:a)
def T.new(a, &b) = b.call(a) if ARGV.size > 5
p T.new(3) { |x| x * 2 }

U = Struct.new(:b)
def U.members(*r) = [:mine, r] if ARGV.size > 5
p U.members
p((U.members(1) rescue [$!.class, $!.message]))
p((U.members(a: 1) rescue [$!.class, $!.message]))
D = Data.define(:x)
def D.members(**k) = :dm if ARGV.size > 5
p D.members
p((D.members(1, 2, z: 3) rescue [$!.class, $!.message]))
E = Data.define(:y)
def E.members(**k) = [:em, k] if ARGV.size < 5
p E.members(q: 1)
W = Struct.new(:a, keyword_init: true)
def W.keyword_init?(*r) = :ki if ARGV.size > 5
p W.keyword_init?
p((W.keyword_init?(1) rescue [$!.class, $!.message]))
