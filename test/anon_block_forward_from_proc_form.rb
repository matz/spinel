# A method reached through a poly dispatch forwards its anonymous `&` into an
# inlined method, which forwards it again to a method that takes its block as
# a proc (both is lowered: its yield sits in a block on a poly receiver).
class Bag
  def initialize(xs) = @xs = xs
  def each(&) = @xs.each(&)
end

class Stereo
  def initialize(outputs) = @outputs = outputs

  def mix(&)
    case @outputs.size
    when 1 then both(@outputs[0], &)
    else pair(@outputs[0], @outputs[1], &)
    end
  end

  def both(samples) = samples.each { |s| yield s; yield s }

  def pair(l, r)
    l.each { |s| yield s }
    r.each { |s| yield s }
  end
end

class BarePlayer
  def initialize(stereo) = @stereo = stereo
  def frame(n, &) = (@stereo.mix(&); n)
end

class MachinePlayer
  def initialize(stereo) = @stereo = stereo
  def frame(n, &) = (@stereo.mix(&); n + 1)
end

class Recorder
  def initialize = @out = []
  attr_reader :out
  def run(pl) = pl.frame(0) { |x| @out << x }
end

r = Recorder.new
[BarePlayer.new(Stereo.new([[1, 2]])), MachinePlayer.new(Stereo.new([Bag.new([3]), [4]]))].each { |pl| r.run(pl) }
p r.out
