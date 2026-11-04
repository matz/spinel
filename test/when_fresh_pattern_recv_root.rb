# spinel: gc-stress
# A `when` pattern read from a fresh object is the only reference to its
# value. The arm allocates while it holds the pattern: a Proc pattern runs
# its own body, which reads its captures after it allocates.

class Holder
  attr_reader :pat

  def initialize(i)
    tag = "cap#{i}" + "w" * 40
    @pat =
      if i % 3 == 0
        i
      else
        ->(x) { parts = (0..20).map { |j| "y#{j}" * 10 }; parts.size > 0 && x.to_s.start_with?(tag[0, 3]) && tag.size > 40 }
      end
  end
end

class StrHolder
  attr_reader :pat

  def initialize(i)
    @pat = i % 3 == 0 ? i : "h#{i}" + "z" * 50
  end
end

def proc_arm(i, flag)
  case (flag ? "cap#{i}" : [i, "x" * 100])
  when Holder.new(i).pat then true
  else false
  end
end

def str_arm(i, flag)
  case (flag ? "h#{i}" + "z" * 50 : [i, "x" * 100])
  when StrHolder.new(i).pat then true
  else false
  end
end

hits = [0, 0]
600.times do |i|
  flag = i.even?
  hits[0] += 1 if proc_arm(i, flag)
  hits[1] += 1 if str_arm(i, flag)
end
p hits
p(case 3 when Holder.new(3).pat then :int else :none end)
p(case "cap1" when Holder.new(1).pat then :proc else :none end)
p(case "nope" when Holder.new(1).pat then :proc else :none end)
