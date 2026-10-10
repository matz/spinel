# A constant bound to an Array literal nothing changes has the literal's
# length: a read at a literal index inside it, or at `x & K` with K below it,
# is an element, and a multiple assignment from it (through `?:`) fills that
# many targets. A read past the end, a nil element, a target past the end, and
# a constant some code changes still answer nil where CRuby does.

module Clock
  DUMMY = [341 - 10, 341, 262]
  BOOT = [341 - 20, 341 * 2, 341 * 2]
  LUT = [4, 8, 16, 32].freeze
  SCALED = [3, 5, 7].map { |n| n * 12 }
  HOLE = [1, nil, 3]
  GROWN = [1, 2]
end

class Machine
  attr_accessor :next_clock
end

class Ppu
  attr_reader :vclk, :hclk_target, :clk, :freq, :edge, :tail, :hole, :grown, :extra

  def initialize(cpu, boot)
    @cpu = cpu
    @vclk, @hclk_target, @cpu.next_clock = boot ? Clock::BOOT : Clock::DUMMY
    @clk = Clock::LUT[0]
    @freq = Clock::SCALED[-1]
    @edge = Clock::LUT[9]
    @tail = Clock::LUT[-5]
    @hole = Clock::HOLE[1]
    @grown = Clock::GROWN[2]
    @a, @b, @c, @extra = Clock::DUMMY
  end

  def step(data)
    @freq = Clock::LUT[data & 3] * 2
    @vclk + @hclk_target + @clk + @freq
  end
end

Clock::GROWN << 99
m = Machine.new
[true, false].each do |boot|
  pp = Ppu.new(m, boot)
  p [pp.vclk, pp.hclk_target, m.next_clock, pp.clk, pp.freq]
  p [pp.edge, pp.tail, pp.hole, pp.grown, pp.extra]
  p [pp.step(5), pp.step(2), pp.step(255)]
end
