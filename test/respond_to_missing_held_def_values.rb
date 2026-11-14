# spinel: int64
# A respond_to_missing? defined under a run-time condition that ran: its
# answer reads big Integers, and allocates when asked many times.
class Big
  if ARGV.empty?
    def respond_to_missing?(n, all = false) = n.to_s.size * 2**40 > 2**42
  end
end
class Idle
  if ARGV.size > 0
    def respond_to_missing?(n, all = false) = n.to_s.size * 2**40 > 2**42
  end
end
p Big.new.respond_to?(:abcde), Big.new.respond_to?(:ab), Idle.new.respond_to?(:abcde)

class Alloc
  if ARGV.empty?
    def respond_to_missing?(n, all = false) = ("dyn" + "_").start_with?(n.to_s[0, 3]) && n.to_s.size > 3
  end
end
ks = Array.new(60) { Alloc.new }
p ks.count { |k| k.respond_to?(:dyn_r) }, ks.count { |k| k.respond_to?(:x) }
