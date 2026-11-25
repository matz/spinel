# spinel: gc-stress
# A fold's step binds its element in a C local of its own, not the method's
# rooted one; the block may take the element out of the array and then
# allocate (or a poll at the step's head may run a trap that does), so the
# element is rooted for the step, through one temp rooted for the fold.
def mk(n) = Array.new(n) { |i| "s" * (i + 40) + i.to_s }

def inject_lens(a)
  a.inject(0) do |s, x|
    a.clear
    $junk = Array.new(20) { |k| "j" * (k + 30) }
    s + x.length
  end
end

def sum_lens(a)
  a.sum { |x| a.clear; $junk = Array.new(20) { |k| "j" * (k + 30) }; x.length }
end

def pair_lens(a)
  a.inject(0) do |s, (k, v)|
    a.clear
    $junk = Array.new(20) { |i| "j" * (i + 30) }
    s + k.length + v.length
  end
end

p inject_lens(mk(5))
p sum_lens(mk(5))
p pair_lens([["k" * 50, "v" * 60], ["a", "b"]])
