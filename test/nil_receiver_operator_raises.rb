# An arithmetic or comparison operator on nil -- a literal, or an instance
# variable nothing sets, which reads nil -- raises CRuby's NoMethodError
# when it runs, after its argument has run; it was a compile refusal.
# nil's own &, | and ^ answer.
class Probe
  def initialize = (@b = 1)

  def ops
    [(nil + 1 rescue $!.message), (nil - 1 rescue $!.message),
     (nil * 2 rescue $!.message), (nil / 2 rescue $!.message),
     (nil % 2 rescue $!.message), (nil ** 2 rescue $!.message),
     (nil < 1 rescue $!.message), (nil <= 1 rescue $!.message),
     (nil > 1 rescue $!.message), (nil >= 1 rescue $!.message),
     nil & true, nil | 1, nil ^ nil]
  end

  def unset = [(@x + 1 rescue $!.class), (@x < 2 rescue $!.class), (@x.size rescue $!.class)]

  def arg_runs
    i = 0
    r = (nil + (i += 1) rescue i)
    [r, i]
  end
end

pr = Probe.new
p pr.ops
p pr.unset
p pr.arg_runs
