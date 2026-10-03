# The search `spinel bisect` runs over a compile's decision keys
# (tools/bisect_search.rb): one culprit among 1 to 5000 keys within the
# probe count it promises, two decisions that are only bad together, a set
# no part of which is bad, a subset that does not build, and the delta
# debugging it falls back on.
require_relative "../tools/bisect_search"

# A program that is wrong when every key of `needs` is allowed, and that does
# not build when `breaks` is allowed without `with`.
class FakeProbe
  def initialize(needs, breaks, with)
    @needs = needs
    @breaks = breaks
    @with = with
  end

  def probe(keys)
    have = {}
    keys.each { |k| have[k] = true }
    return BS_SKIP if @breaks.length > 0 && have.key?(@breaks) && !have.key?(@with)
    @needs.each { |k| return BS_GOOD if !have.key?(k) }
    BS_BAD
  end
end

def log2_ceil(n)
  b = 0
  v = 1
  while v < n
    v *= 2
    b += 1
  end
  b
end

# `n` keys of each of `kinds`, in the order a compile would log them.
def keys_of_kinds(kinds, n)
  keys = []
  i = 0
  while i < n
    kinds.each { |kd| keys.push(kd + "@app.rb:" + (i + 1).to_s + ":1") }
    i += 1
  end
  keys
end

def search(keys, needs, breaks = "", with = "")
  s = BisectSearch.new(FakeProbe.new(needs, breaks, with))
  # what the tool has built before it searches: none allowed, and all
  s.know([], BS_GOOD)
  s.know(keys, BS_BAD)
  found = s.run(keys)
  [found, s.calls, s.skips]
end

puts bs_kind("nn-read@app.rb:12:5:x")
puts bs_site("nn-read@app.rb:12:5:x")
puts bs_kind("root-elide@Lut#load:@lut")
puts bs_site("root-elide@Lut#load:@lut")
puts bs_site("root-frame@main")
puts bs_place("gc-save@Count#to_int")
puts bs_place("root-elide@Lut#load:@lut")
puts bs_place("nn-read@app.rb:12:5:x")
puts bs_place("root-frame@main")
puts bs_place("root-elide@Lut.load:@lut")
puts bs_place("root-elide@main:x")
p bs_at_places(["a@K#m", "b@app.rb:3:1", "c@K#m:@x", "d@K#n"], ["z@K#m"])
p bs_kinds(["b@1", "a@1", "b@2", "c@1", "a@2"])
p bs_of_kinds(["b@1", "a@1", "b@2", "c@1"], ["c", "b"])
p bs_without(["b@1", "a@1", "b@2"], ["a@1"])

# one culprit among N keys: found, and within log2 N + 1 builds
[1, 2, 64, 1000].each do |n|
  [["nn-read"], ["no-alloc", "nn-read", "root-frame", "gc-save", "pd-hoist"]].each do |kinds|
    keys = keys_of_kinds(kinds, n)
    culprit = keys[(keys.length * 2) / 3]
    found, calls, _skips = search(keys, [culprit])
    bound = log2_ceil(keys.length) + 1
    puts n.to_s + " x " + kinds.length.to_s + ": " + (found == [culprit] ? "found" : "MISSED") +
         (calls <= bound ? " within " : " OVER ") + bound.to_s
  end
end

# two decisions that are only wrong together: one kind, then two
keys = keys_of_kinds(["nn-read", "no-alloc", "root-frame"], 20)
found, _calls, _skips = search(keys, ["nn-read@app.rb:3:1", "nn-read@app.rb:17:1"])
p found
found, _calls, _skips = search(keys, ["root-frame@app.rb:9:1", "nn-read@app.rb:17:1"])
p found

# ... and two at one place, a method's frame and the save that pops it: the
# second is looked for at the place of the first, not among all the keys
keys = keys_of_kinds(["nn-read", "no-alloc", "root-frame"], 100)
keys.insert(40, "root-frame@Lut#load")
keys.push("gc-save@Lut#load")
found, calls, _skips = search(keys, ["root-frame@Lut#load", "gc-save@Lut#load"])
p found
puts calls <= log2_ceil(keys.length) + 4

# nothing smaller is wrong: the whole set comes back
keys = keys_of_kinds(["nn-read", "gc-save"], 3)
found, _calls, _skips = search(keys, keys)
puts found == keys

# a key that does not build without its partner: still found, the skips counted
keys = keys_of_kinds(["nn-read", "no-alloc"], 16)
found, _calls, skips = search(keys, ["nn-read@app.rb:5:1"], "nn-read@app.rb:2:1", "nn-read@app.rb:11:1")
p found
puts skips > 0
# ... and when the culprit is the key that does not build alone, nothing
# smaller can be tested: the whole set comes back with the skip counted
keys = keys_of_kinds(["nn-read"], 2)
found, _calls, skips = search(keys, ["nn-read@app.rb:1:1"], "nn-read@app.rb:1:1", "nn-read@app.rb:2:1")
puts found == keys
puts skips

# an answer the caller already has costs no probe
s = BisectSearch.new(FakeProbe.new(["a@1"], "", ""))
s.know(["a@1", "b@1"], BS_BAD)
puts s.ask(["b@1", "a@1"])
puts s.calls
puts s.ask(["b@1"])
puts s.calls

# delta debugging, which run falls back on when what it found is not wrong
# on its own: over keys, then over kinds and the keys of the kinds left
keys = keys_of_kinds(["nn-read", "no-alloc", "root-frame"], 8)
s = BisectSearch.new(FakeProbe.new(["no-alloc@app.rb:3:1", "root-frame@app.rb:6:1"], "", ""))
p s.ddmin(keys, [])
kinds = s.ddmin(bs_kinds(keys), keys)
p kinds
p s.ddmin(bs_of_kinds(keys, kinds), [])
