# A prepended forward with keyword parameters and no keyword rest, given a
# literal keyword it does not name: CRuby's binding raises `unknown keyword`
# after the call's arguments run and before the forward's own defaults do,
# once the forwards ahead of it have run theirs; whatever the builtin, and
# wherever the call stands.
$log = []
module Sized
  def initialize(d = ($log << :d; 3), capacity: 1) = super
end
Hash.prepend(Sized)
begin; Hash.new(foo: 1); rescue ArgumentError => e; p e.message; end
begin; Hash.new(capacity: 2, foo: 1); rescue ArgumentError => e; p e.message; end
begin; Hash.new(foo: 1, bar: 2); rescue ArgumentError => e; p e.message; end
begin; Hash.new("a-b": 1); rescue ArgumentError => e; p e.message; end
begin; Hash.new(($log << :a; 5), foo: ($log << :f; 1)); rescue ArgumentError => e; p e.message; end
p $log
x = (Hash.new(foo: 1) rescue { r: 1 })
p x
p [(Hash.new(foo: 1) rescue 1), (Hash.new(bar: 2) rescue 2)]
def mk = Hash.new(foo: 1)
p((mk rescue $!.class))
pr = proc { |hh, k| k }
p((Hash.new(foo: 1, &pr) rescue $!.message))
p Hash.new(capacity: 2)[:x], $log

module Counted
  def initialize(n = 2, k: 1) = super
end
Array.prepend(Counted)
p((Array.new(foo: 1) rescue $!.message))
p((Array.new(3, foo: 1, k: 2) rescue $!.message))
