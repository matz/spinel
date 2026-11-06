# A Method of an accessor that its class declares again under a condition
# stands at the last declaration sure to run, as CRuby answers when the
# condition is false (this test's argument). Whether the conditional one
# ran is not known before the program runs.
class C
  attr_reader :x
end
class C
  attr_reader :x if ARGV.empty?
end
m = C.new.method(:x)
p m.source_location, m.owner
o = [C.new, 1][0]
p o.method(:x).source_location
